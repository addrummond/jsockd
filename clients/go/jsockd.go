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
	"regexp"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

// JSockDVersion specifies the expected version of JSockD [__jsockd_version_check__ <- for automatic CI check]
const JSockDVersion = "0.0.102"

const chanBufferSize = 64

var logLineRegex = regexp.MustCompile(`(\*|\$) jsockd ([^ ]+) \[([^][]+)\] (.*)`)

var nextCommandId uint64

type command struct {
	id           string
	query        string
	paramJson    string
	responseChan chan RawResponse
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
	cmd             *exec.Cmd
	conns           []net.Conn
	connChans       []chan command
	config          Config
	quitCount       uint64
	fatalError      error
	fatalErrorMutex sync.Mutex
}

// InitJSockDClient starts a JSockD server process with the specified
// configuration, connects to it via the specified Unix domain sockets, and
// returns a JSockDClient that can be used to send commands to the server.
func InitJSockDClient(config Config, jsockdExec string, sockets []string) (*JSockDClient, error) {
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

	if jsockdExec == "" {
		return nil, errors.New("JSockDExec not set in Config")
	}

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
		return nil, fmt.Errorf("stdout pipe: %w", err)
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
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil, fmt.Errorf("waiting for READY: %w", e)
	case <-time.After(timeout):
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
	client := &JSockDClient{
		conns:     conns,
		connChans: connChans,
		config:    config,
		cmd:       cmd,
	}

	for i := range readyCount {
		connChans[i] = make(chan command, chanBufferSize)
		go connHandler(conns[i], connChans[i], client)
	}

	return client, nil
}

// SendCommand sends a command to the JSockD server and returns the
// response. The parameter is serialized to JSON via json.Marshal and the result
// is deserialized from JSON via json.Unmarshal.
func SendCommand[ResponseT any](client *JSockDClient, query string, jsonParam any) (Response[ResponseT], error) {
	j, err := json.Marshal(jsonParam)
	if err != nil {
		return Response[ResponseT]{}, fmt.Errorf("json marshal: %w", err)
	}
	rawResp, err := SendRawCommand(client, query, string(j))
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
	if fe := getFatalError(client); fe != nil {
		return RawResponse{}, fe
	}

	cmdId := atomic.AddUint64(&nextCommandId, 1)
	cmd := command{
		id:           strconv.FormatUint(cmdId, 10),
		query:        query,
		paramJson:    jsonParam,
		responseChan: make(chan RawResponse),
	}
	nconns := len(client.conns)
	if nconns == 0 {
		return RawResponse{}, errors.New("no connections available")
	}

	chooseChan(client.connChans) <- cmd

	select {
	case resp := <-cmd.responseChan:
		return resp, nil
	case <-time.After(time.Duration(client.config.TimeoutUs) * time.Microsecond):
		return RawResponse{}, errors.New("timeout waiting for response")
	}
}

// Close closes all connections to the JSockD server, all channels used
// internally by the JDockD client code, and waits for the JSockD process to
// terminate. Close may be called multiple times without ill effect; subsequent
// calls are no-ops.
func (client *JSockDClient) Close() error {
	qs := atomic.AddUint64(&client.quitCount, 1)
	if qs > 1 {
		// Already quitting or quit
		return nil
	}

	for _, c := range client.connChans {
		close(c)
	}
	var err error
	for _, c := range client.conns {
		if connErr := c.Close(); err != nil && connErr != nil {
			err = connErr
		}
	}
	client.cmd.WaitDelay = time.Duration(client.config.TimeoutUs) * time.Microsecond
	err2 := client.cmd.Wait()
	if err != nil {
		return err
	}
	if err2 != nil {
		return err2
	}
	if fe := getFatalError(client); fe != nil {
		return fe
	}
	return nil
}

func connHandler(conn net.Conn, cmdChan chan command, client *JSockDClient) {
	defer conn.Close()

	for cmd := range cmdChan {
		_, err := conn.Write(fmt.Appendf(nil, "%s\x00%s\x00%s\x00", cmd.id, cmd.query, cmd.paramJson))
		if err != nil {
			setFatalError(client, err)
			return
		}
		rec, err := readRecord(conn)
		if err != nil {
			setFatalError(client, err)
			return
		}
		parts := strings.SplitN(rec, " ", 2)
		commandId := parts[0]
		if commandId != cmd.id {
			setFatalError(client, fmt.Errorf("mismatched command id: got %q, wanted %q", commandId, cmd.id))
			return
		}
		if len(parts) != 2 {
			setFatalError(client, fmt.Errorf("malformed response record: %q", rec))
			return
		}
		if strings.HasPrefix(parts[1], "exception ") {
			cmd.responseChan <- RawResponse{Exception: true, ResultJson: strings.TrimPrefix(parts[1], "exception ")}
		} else {
			cmd.responseChan <- RawResponse{Exception: false, ResultJson: parts[1]}
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
			currentLine.WriteByte('\n')
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

func getFatalError(client *JSockDClient) error {
	client.fatalErrorMutex.Lock()
	defer client.fatalErrorMutex.Unlock()
	return client.fatalError
}

func setFatalError(client *JSockDClient, err error) {
	client.fatalErrorMutex.Lock()
	defer client.fatalErrorMutex.Unlock()
	if client.fatalError == nil {
		client.fatalError = err
	}
}
