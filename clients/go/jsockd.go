package jsockdclient

import (
	"bufio"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"os/exec"
	"strconv"
	"strings"
	"time"
)

const chanBufferSize = 64

var nextCommandId uint64 = 0

type Command struct {
	Id        string
	Query     string
	ParamJson string
}

type Response struct {
	Exception  bool
	ResultJson string
}

type JSockDClient struct {
	conns        []net.Conn
	connChannels []chan Command
}

func InitJSockD(config Config, sockets []string) (*JSockDClient, error) {
	cmdargs := []string{"-b", "00"}
	if config.BytecodeModuleFile != "" {
		cmdargs = append(cmdargs, "-m", config.BytecodeModuleFile)
	}
	if config.SourceMap != "" {
		cmdargs = append(cmdargs, "-sm", config.SourceMap)
	}
	if config.MaxIdleTimeUs != -1 {
		cmdargs = append(cmdargs, "-i", strconv.Itoa(config.MaxIdleTimeUs))
	}
	if config.MaxCommandRuntimeUs != -1 {
		cmdargs = append(cmdargs, "-t", strconv.Itoa(config.MaxCommandRuntimeUs))
	}

	cmdargs = append(cmdargs, "-s", "--")
	cmdargs = append(cmdargs, sockets...)

	if config.JSockDExec == "" {
		return nil, errors.New("JSockDExec not set in Config")
	}

	// Start the process
	cmd := exec.Command(config.JSockDExec, cmdargs...)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("stdout pipe: %w", err)
	}
	// TODO stderr handling
	//stderr, err := cmd.StderrPipe()
	//if err != nil {
	//	return nil, fmt.Errorf("stderr pipe: %w", err)
	//}

	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start %s: %w", config.JSockDExec, err)
	}

	readyCh := make(chan int, 1)
	errCh := make(chan error, 1)

	go func(r io.Reader) {
		scanner := bufio.NewScanner(r)
		for scanner.Scan() {
			line := strings.TrimSpace(scanner.Text())
			if strings.HasPrefix(line, "READY ") {
				parts := strings.SplitN(line, " ", 2)
				if len(parts) == 2 {
					n, convErr := strconv.Atoi(strings.TrimSpace(parts[1]))
					if convErr == nil && n > 0 {
						readyCh <- n
						return
					}
					errCh <- fmt.Errorf("malformed READY line: %q", line)
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
	}(stdout)

	timeout := time.Duration(config.TimeoutUs) * time.Microsecond

	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()

	var readyCount int
	select {
	case readyCount = <-readyCh:
		// success
	case e := <-errCh:
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil, fmt.Errorf("waiting for READY: %w", e)
	case <-ctx.Done():
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil, fmt.Errorf("timeout (%s) waiting for READY line", timeout)
	}

	if readyCount < len(sockets) {
		return nil, fmt.Errorf("ready count (%d) exceeds number of sockets specified (%d)", readyCount, len(sockets))
	}

	conns := make([]net.Conn, 0, readyCount)
	for i := 0; i < readyCount; i++ {
		sock := sockets[i]
		conn, err := net.DialTimeout("unix", sock, timeout)
		if err != nil {
			_ = cmd.Process.Kill()
			_ = cmd.Wait()
			return nil, fmt.Errorf("dial %s: %w", sock, err)
		}
		defer conn.Close()
		conns = append(conns, conn)
	}

	connChans := make([]chan Command, readyCount)
	for i := 0; i < readyCount; i++ {
		connChans[i] = make(chan Command, chanBufferSize)
		go connHandler(conns[i], connChans[i])
	}

	return &JSockDClient{conns: conns}, nil
}

func connHandler(conn net.Conn, cmdChan chan Command, responseChan chan Response) {
	defer conn.Close()

	for cmd := range cmdChan {
		_, err := conn.Write(fmt.Appendf(nil, "id\x00%s\x00%s\x00", cmd.Query, cmd.ParamJson))
		if err != nil {
			panic("TODO ERROR HANDLING")
		}
		rec, err := readRecord(conn)
		parts := strings.SplitN(rec, " ", 2)
		commandId := parts[0]
		if commandId != cmd.Id {
			// TODO error handling
		}
		if len(parts) != 2 {
			panic("TODO ERROR HANDLING")
		}
		if strings.HasPrefix(parts[1], "exception ") {
			responseChan <- Response{Exception: true, ResultJson: strings.TrimPrefix(parts[1], "exception ")}
		} else {
			responseChan <- Response{Exception: false, ResultJson: parts[1]}
		}
	}
}

func readRecord(conn net.Conn) (string, error) {
	r := bufio.NewReader(conn)
	record, err := r.ReadString('\x00')
	if err != nil {
		return "", err
	}
	return record, nil
}
