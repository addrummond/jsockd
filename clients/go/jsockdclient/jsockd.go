// Package jsockdclient provides a Go client for communicating with a
// jsockd server.
//
// See https://github.com/addrummond/jsockd for general information about
// JSockD.
//
// Example usage:
// ```go
// config := jsockdclient.DefaultConfig()
//
//	config.Logger = func(timestamp time.Time, level, message string) {
//	  fmt.Printf("%s [%s] %s\n", timestamp.Format(time.RFC3339Nano), level, message)
//	}
//
// client, err := jsockdclient.InitJSockD(config, "/path/to/jsockd", []string{"/tmp/jsockd.sock1", "/tmp/jsockd.sock2"})
//
//	if err != nil {
//	  log.Fatal(err)
//	}
//
// defer client.Close()
// response, err := jsockdclient.SendRawCommand(client, "(m, p) => p+1", "99")
//
//	if err != nil {
//	  log.Fatal(err)
//	}
//
// fmt.Printf("%+v\n", response)
// ```
package jsockdclient

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math/rand"
	"net"
	"os"
	"os/exec"
	"path"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// JSockDVersion specifies the expected version of JSockD [__jsockd_version_check__ <- for automatic CI check]
const JSockDVersion = "0.0.109"

const chanBufferSize = 64

var logLineRegex = regexp.MustCompile(`(\*|\$) jsockd ([^ ]+) \[([^][]+)\] (.*)`)

var nextCommandId uint64

type command struct {
	id             string
	query          string
	paramJson      string
	responseChan   chan RawResponse
	messageHandler func(jsonMessage string) (string, error)
}

// RawResponse represents the raw response to a command sent to the JSockD
// server. The ResultJson field contains the JSON string returned by the server.
type RawResponse struct {
	Exception  bool
	ResultJson string
}

// Response represents the response to a command sent to the JSockD server. The
// Result field is of the type specified by the caller when sending the command.
type Response[T any] struct {
	Exception bool
	Result    T
}

// JSockDClient represents a client connection to a JSockD server. It should
// be created via InitJSockDClient and closed via its Close method.
type JSockDClient struct {
	// We go indirectly via an atomic pointer to the real client state so that
	// we can swap out the pointer in the case where jsockd is restarted.
	iclient atomic.Pointer[jSockDInternalClient]
}

type jSockDInternalClient struct {
	cmd             *exec.Cmd
	conns           []net.Conn
	connChans       []chan command
	config          Config
	quitCount       uint64
	fatalError      error
	fatalErrorMutex sync.Mutex
	socketTmpdir    string
	restartCount    atomic.Int64
	lastRestart     time.Time
	process         *os.Process
}

const messageHandlerInternalError = "internal_error"

// GetJSockDProcess returns the underlying JSockD process for the client.
// You wouldn't usually need to use this.
func (client *JSockDClient) GetJSockDProcess() *os.Process {
	iclient := client.iclient.Load()
	return iclient.process
}

// InitJSockDClientWithSockets starts a JSockD server process with the specified
// configuration, connects to it via the specified Unix domain sockets, and
// returns a JSockDClient that can be used to send commands to the server.
func InitJSockDClient(config Config, jsockdExec string) (*JSockDClient, error) {
	if config.NThreads <= 0 {
		return nil, errors.New("NThreads must be greater than zero. Did you forget to call jsockdclient.DefaultConfig() to initialize the Config struct?")
	}
	client, err := initJSockDClient(config, jsockdExec)
	c := client.iclient.Load()
	if err != nil && client != nil && c.socketTmpdir != "" {
		// Ignore error as it's just nice to have cleanup
		os.RemoveAll(c.socketTmpdir)
	}
	return client, err
}

func initJSockDClient(config Config, jsockdExec string) (*JSockDClient, error) {
	// We set this before killing the jsockd process ourselves, so that any code Waiting on it knows if it was killed deliberately.
	var jsockdKillCount atomic.Int32

	var sockets []string
	var socketTmpdir string
	if config.Sockets != nil {
		sockets = config.Sockets
	} else {
		var err error
		// We want to use a short name here becasue Unix domain socket path
		// length limits can be quite small on some systems, and the
		// temporary dir may have quite a long path.
		socketTmpdir, err = os.MkdirTemp("", "jsd_")
		if err != nil {
			return nil, err
		}
		sockets = make([]string, config.NThreads)
		for i := 0; i < config.NThreads; i++ {
			sockets[i] = path.Join(socketTmpdir, fmt.Sprintf("jsd_%d.sock", i))
		}
	}

	cmdargs := prepareCmdArgs(config, jsockdExec, sockets)

	// Start the process
	cmd := exec.Command(jsockdExec, cmdargs...)
	cmd.Env = os.Environ()
	if config.BytecodeModulePublicKey != "" {
		cmd.Env = append(cmd.Env, fmt.Sprintf("JSOCKD_BYTECODE_MODULE_PUBLIC_KEY=%s", config.BytecodeModulePublicKey))
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("stdout pipe: %w", err)
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return nil, fmt.Errorf("stderr pipe: %w", err)
	}

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start %s: %w", jsockdExec, err)
	}

	readyCh := make(chan int, 1)
	errCh := make(chan error, 1)

	go readReadyFromStdout(stdout, readyCh, errCh, config)
	go streamStderrLogs(stderr, config)

	timeout := time.Duration(config.TimeoutUs) * time.Microsecond

	var readyCount int
	select {
	case readyCount = <-readyCh:
		// success
	case e := <-errCh:
		jsockdKillCount.Add(1)
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil, fmt.Errorf("waiting for READY: %w", e)
	case <-time.After(timeout):
		jsockdKillCount.Add(1)
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil, fmt.Errorf("timeout (%s) waiting for READY line", timeout)
	}

	if readyCount > len(sockets) {
		return nil, fmt.Errorf("ready count (%d) exceeds number of sockets specified (%d)", readyCount, len(sockets))
	}

	conns := make([]net.Conn, 0, readyCount)
	for i := range readyCount {
		sock := sockets[i]
		conn, err := net.DialTimeout("unix", sock, timeout)
		if err != nil {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
			return nil, fmt.Errorf("dial %s: %w", sock, err)
		}
		conns = append(conns, conn)
	}

	connChans := make([]chan command, readyCount)
	iclient := &jSockDInternalClient{
		conns:        conns,
		connChans:    connChans,
		config:       config,
		cmd:          cmd,
		socketTmpdir: socketTmpdir,
		process:      cmd.Process,
	}

	for i := range readyCount {
		connChans[i] = make(chan command, chanBufferSize)
		go connHandler(conns[i], connChans[i], iclient)
	}

	client := &JSockDClient{}
	client.iclient.Store(iclient)

	// Get notified when the process exits
	go func() {
		_, _ = cmd.Process.Wait()
		if jsockdKillCount.Load() == 0 {
			setFatalError(iclient, fmt.Errorf("jsockd process exited unexpectedly with code %v", cmd.ProcessState.ExitCode()))
		}
		fmt.Fprintf(os.Stderr, "JSockD process exited\n")
		minutesPassed := 0
		if !iclient.lastRestart.IsZero() {
			minutesPassed = int(time.Since(iclient.lastRestart).Minutes())
		}
		if iclient.restartCount.Add(1)-int64(minutesPassed*config.MaxRestartsPerMinute) <= int64(config.MaxRestartsPerMinute) {
			fmt.Fprintf(os.Stderr, "JSockD process exited in here!\n")
			iclient.lastRestart = time.Now()
			newClient, err := initJSockDClient(config, jsockdExec)
			newIclient := newClient.iclient.Load()
			newIclient.lastRestart = time.Now()
			if err != nil {
				setFatalError(newIclient, fmt.Errorf("restarting jsockd: %w", err))
				return
			}
			client.iclient.Store(newIclient)
			fmt.Fprintf(os.Stderr, "JSockD process restarted!!\n")
		}
	}()

	return client, nil
}

// SendCommand sends a command to the JSockD server and returns the
// response. The parameter is serialized to JSON via json.Marshal and the result
// is deserialized from JSON via json.Unmarshal.
func SendCommand[ResponseT any](client *JSockDClient, query string, jsonParam any) (Response[ResponseT], error) {
	return sendCommand[ResponseT](client.iclient.Load(), query, jsonParam, nil)
}

// SendCommandWithMessageHandler is like SendCommand but with a message
// handling function as an additional parameter. If there is an error
// JSON-decoding the `message` argument of the message handler, or if
// there is an error JSON-encoding its return value, then null will be
// sent as the response to the JSockD server, and
// SendCommandWithMessageHandler will return a non-nil error.
//
// Note that messageHandler, when called, will execute in a different
// goroutine to the one that called SendCommandWithMessageHandler. This
// goroutine is guaranteed to have finished executing by the time
// SendCommandWithMessageHandler returns.
func SendCommandWithMessageHandler[ResponseT any, MessageT any, MessageResponseT any](client *JSockDClient, query string, jsonParam any, messageHandler func(message MessageT) (MessageResponseT, error)) (Response[ResponseT], error) {
	fmt.Printf("SendCommandWithMessageHandler %p\n", client.iclient.Load())

	var msgHandlerErr error
	wrappedHandler := func(jsonMessage string) (string, error) {
		var message MessageT
		msgHandlerErr = json.Unmarshal([]byte(jsonMessage), &message)
		if msgHandlerErr != nil {
			return "", msgHandlerErr
		}
		var response MessageResponseT
		response, msgHandlerErr = messageHandler(message)
		if msgHandlerErr != nil {
			return "", msgHandlerErr
		}
		j, msgHandlerErr := json.Marshal(response)
		if msgHandlerErr != nil {
			return "", msgHandlerErr
		}
		return string(j), nil
	}
	res, err := sendCommand[ResponseT](client.iclient.Load(), query, jsonParam, wrappedHandler)
	if err != nil && msgHandlerErr != nil {
		return Response[ResponseT]{}, fmt.Errorf("message handler error: %w; command error: %v", msgHandlerErr, err)
	}
	if err != nil {
		return Response[ResponseT]{}, err
	}
	if msgHandlerErr != nil {
		return Response[ResponseT]{}, fmt.Errorf("message handler error: %w", msgHandlerErr)
	}
	return res, nil
}

func sendCommand[ResponseT any](iclient *jSockDInternalClient, query string, jsonParam any, messageHandler func(jsonMessage string) (string, error)) (Response[ResponseT], error) {
	j, err := json.Marshal(jsonParam)
	if err != nil {
		return Response[ResponseT]{}, fmt.Errorf("json marshal: %w", err)
	}
	rawResp, err := sendRawCommand(iclient, query, string(j), messageHandler)
	if err != nil {
		return Response[ResponseT]{}, err
	}
	var resp Response[ResponseT]
	resp.Exception = rawResp.Exception
	if !rawResp.Exception {
		err = json.Unmarshal([]byte(rawResp.ResultJson), &resp.Result)
		if err != nil {
			return Response[ResponseT]{}, fmt.Errorf("json unmarshal: %w", err)
		}
	} else {
		resp.Result = *new(ResponseT)
	}
	return resp, nil
}

// SendRawCommand sends a command to the JSockD server and returns the
// response. JSON values are passed and returned as strings.
func SendRawCommand(client *JSockDClient, query string, jsonParam string) (RawResponse, error) {
	return sendRawCommand(client.iclient.Load(), query, jsonParam, nil)
}

// SendRawCommandWithMessageHandler is like SendRawCommand but with a message
// handling function as an additional parameter. The message handling function
// receives JSON-encoded messages and should return a valid JSON-encoded
// response.
//
// Note that messageHandler, when called, will execute in a different
// goroutine to the one that called SendRawCommandWithMessageHandler. This
// goroutine is guaranteed to have finished executing by the time
// SendRawCommandWithMessageHandler returns.
func SendRawCommandWithMessageHandler(client *JSockDClient, query string, jsonParam string, messageHandler func(jsonMessage string) (string, error)) (RawResponse, error) {
	iclient := client.iclient.Load()
	return sendRawCommand(iclient, query, jsonParam, messageHandler)
}

func sendRawCommand(iclient *jSockDInternalClient, query string, jsonParam string, messageHandler func(jsonMessage string) (string, error)) (RawResponse, error) {
	if fe := getFatalError(iclient); fe != nil {
		return RawResponse{}, fe
	}

	fmt.Printf("SEND RAW %p\n", iclient)

	cmdId := atomic.AddUint64(&nextCommandId, 1)
	cmd := command{
		id:             strconv.FormatUint(cmdId, 10),
		query:          query,
		paramJson:      jsonParam,
		responseChan:   make(chan RawResponse),
		messageHandler: messageHandler,
	}
	nconns := len(iclient.conns)
	if nconns == 0 {
		return RawResponse{}, errors.New("no connections available")
	}

	chooseChan(iclient.connChans) <- cmd

	select {
	case resp := <-cmd.responseChan:
		return resp, nil
	case <-time.After(time.Duration(iclient.config.TimeoutUs) * time.Microsecond):
		return RawResponse{}, errors.New("timeout waiting for response")
	}
}

// Close closes all connections to the JSockD server, all channels used
// internally by the JDockD client code, and waits for the JSockD process to
// terminate. Close may be called multiple times without ill effect; subsequent
// calls are no-ops.
func (client *JSockDClient) Close() error {
	iclient := client.iclient.Load()

	qs := atomic.AddUint64(&iclient.quitCount, 1)
	if qs > 1 {
		// Already quitting or quit
		return nil
	}

	// Ignore error as it's just nice to have cleanup
	if iclient.socketTmpdir != "" {
		defer os.RemoveAll(iclient.socketTmpdir)
	}

	for _, c := range iclient.connChans {
		close(c)
	}
	var err error
	for _, c := range iclient.conns {
		if connErr := c.Close(); err != nil && connErr != nil {
			err = connErr
		}
	}
	iclient.cmd.WaitDelay = time.Duration(iclient.config.TimeoutUs) * time.Microsecond
	err2 := iclient.cmd.Wait()
	if err != nil {
		return err
	}
	if err2 != nil {
		return err2
	}
	if fe := getFatalError(iclient); fe != nil {
		return fe
	}
	return nil
}

func connHandler(conn net.Conn, cmdChan chan command, iclient *jSockDInternalClient) {
	defer conn.Close()

	for cmd := range cmdChan {
		_, err := conn.Write(fmt.Appendf(nil, "%s\x00%s\x00%s\x00", cmd.id, cmd.query, cmd.paramJson))
		if err != nil {
			setFatalError(iclient, err)
			return
		}
		rec, err := readRecord(conn)
		if err != nil {
			setFatalError(iclient, err)
			return
		}
		parts := strings.SplitN(rec, " ", 2)
		commandId := parts[0]
		if commandId != cmd.id {
			setFatalError(iclient, fmt.Errorf("mismatched command id: got %q, wanted %q", commandId, cmd.id))
			return
		}
		if len(parts) != 2 {
			setFatalError(iclient, fmt.Errorf("malformed response record: %q", rec))
			return
		}
		if rest, ok := strings.CutPrefix(parts[1], "exception "); ok {
			cmd.responseChan <- RawResponse{Exception: true, ResultJson: rest}
		} else if rest, ok := strings.CutPrefix(parts[1], "ok "); ok {
			cmd.responseChan <- RawResponse{Exception: false, ResultJson: rest}
		} else if rest, ok := strings.CutPrefix(parts[1], "message "); ok {
			for {
				response := "null"
				err := errors.New("internal error: no message handler")
				if cmd.messageHandler != nil {
					response, err = cmd.messageHandler(strings.TrimSuffix(rest, "\n"))
				}
				if err != nil {
					setFatalError(iclient, fmt.Errorf("message handler error: %w", err))
					_, _ = conn.Write(fmt.Appendf(nil, "%s\x00%s\x00", cmd.id, messageHandlerInternalError))
					return
				}
				_, err = conn.Write(fmt.Appendf(nil, "%s\x00%s\x00", cmd.id, response))
				if err != nil {
					setFatalError(iclient, err)
					return
				}
				mresp, err := readRecord(conn)
				if err != nil {
					setFatalError(iclient, err)
					return
				}
				parts = strings.SplitN(mresp, " ", 2)
				if parts[0] != cmd.id {
					setFatalError(iclient, fmt.Errorf("mismatched command id in message response: got %q, wanted %q", parts[0], cmd.id))
					return
				}
				var ok bool
				if rest, ok = strings.CutPrefix(parts[1], "ok "); ok {
					cmd.responseChan <- RawResponse{Exception: false, ResultJson: strings.TrimSuffix(rest, "\n")}
					break
				} else if rest, ok = strings.CutPrefix(parts[1], "exception "); ok {
					cmd.responseChan <- RawResponse{Exception: true, ResultJson: strings.TrimSuffix(rest, "\n")}
					return
				} else if rest, ok = strings.CutPrefix(parts[1], "message "); ok {
					continue
				} else {
					setFatalError(iclient, fmt.Errorf("malformed message response from JSockD: %q", mresp))
					return
				}
			}
		} else {
			setFatalError(iclient, fmt.Errorf("malformed command response from JSockD: %q", rec))
			return
		}
	}
}

func readReadyFromStdout(stdout io.Reader, readyCh chan<- int, errCh chan<- error, config Config) {
	scanner := bufio.NewScanner(stdout)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if strings.HasPrefix(line, "READY ") {
			parts := strings.SplitN(line, " ", 3)
			if len(parts) == 3 {
				n, convErr := strconv.Atoi(strings.TrimSpace(parts[1]))
				if convErr != nil || n <= 0 {
					errCh <- fmt.Errorf("malformed READY line: %q", line)
					return
				}
				if !config.SkipJSockDVersionCheck && parts[2] != JSockDVersion {
					errCh <- fmt.Errorf("version mismatch: client %q, server %q", JSockDVersion, parts[2])
					return
				}
				readyCh <- n
				return
			}
			errCh <- fmt.Errorf("malformed READY line: %q", line)
			return
		}
	}
	if scanErr := scanner.Err(); scanErr != nil {
		errCh <- scanErr
	} else {
		errCh <- errors.New("stdout closed before READY line")
	}
}

func streamStderrLogs(stderr io.Reader, config Config) {
	var zeroTime time.Time

	scanner := bufio.NewScanner(stderr)
	var currentLine strings.Builder
	lastWasDollar := true
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		groups := logLineRegex.FindStringSubmatch(line)
		if len(groups) != 5 {
			config.Logger(zeroTime, "INFO", line)
			continue
		}
		prefix := string(groups[1])
		timestamp := string(groups[2])
		level := string(groups[3])
		msg := string(groups[4])

		t, err := time.Parse(time.RFC3339Nano, timestamp)
		if err != nil {
			config.Logger(zeroTime, level, msg)
			continue
		}

		if prefix == "*" {
			if currentLine.Len() > 0 {
				currentLine.WriteByte('\n')
			}
			currentLine.WriteString(msg)
		} else if prefix == "$" {
			if !lastWasDollar {
				lastWasDollar = false
				currentLine.WriteByte('\n')
			}
			currentLine.WriteString(msg)
			if config.Logger != nil {
				config.Logger(t, level, currentLine.String())
			}
			currentLine.Reset()
		} else {
			config.Logger(zeroTime, "INFO", line)
		}
	}
}

func readRecord(conn net.Conn) (string, error) {
	r := bufio.NewReader(conn)
	record, err := r.ReadString('\n')
	if err != nil {
		return "", err
	}
	return record, nil
}

func chooseChan(connChans []chan command) chan command {
	for i := range len(connChans) {
		if len(connChans[i]) == 0 {
			return connChans[i]
		}
	}
	return connChans[rand.Intn(len(connChans))]
}

func getFatalError(iclient *jSockDInternalClient) error {
	iclient.fatalErrorMutex.Lock()
	defer iclient.fatalErrorMutex.Unlock()
	return iclient.fatalError
}

func setFatalError(iclient *jSockDInternalClient, err error) {
	iclient.fatalErrorMutex.Lock()
	defer iclient.fatalErrorMutex.Unlock()
	if iclient.fatalError == nil {
		iclient.fatalError = err
	}
}

func prepareCmdArgs(config Config, jsockdExec string, sockets []string) []string {
	cmdargs := []string{"-b", "00"}
	if config.BytecodeModuleFile != "" {
		cmdargs = append(cmdargs, "-m", config.BytecodeModuleFile)
	}
	if config.SourceMap != "" {
		cmdargs = append(cmdargs, "-sm", config.SourceMap)
	}
	if config.MaxIdleTimeUs != 0 {
		cmdargs = append(cmdargs, "-i", strconv.Itoa(config.MaxIdleTimeUs))
	}
	if config.MaxCommandRuntimeUs != 0 {
		cmdargs = append(cmdargs, "-t", strconv.Itoa(config.MaxCommandRuntimeUs))
	}

	cmdargs = append(cmdargs, "-s", "--")
	cmdargs = append(cmdargs, sockets...)
	return cmdargs
}
