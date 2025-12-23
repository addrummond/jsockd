package jsockdclient

import (
	"fmt"
	"os"
	"strings"
	"sync"
	"testing"
)

func TestSendRawCommand(t *testing.T) {
	t.Run("good command", func(t *testing.T) {
		config := DefaultConfig()
		config.SkipJSockDVersionCheck = true
		config.NThreads = 1
		//config.Logger = logger
		client, err := InitJSockDClient(config, getJSockDPath(t))
		if err != nil {
			t.Fatal(err)
		}
		defer client.Close()
		if err != nil {
			t.Fatal(err)
		}
		response, err := SendRawCommand(client, "(m, p) => p+1", "99")
		if response.Exception {
			t.Fatalf("Exception: %s", response.ResultJson)
		}
		if strings.TrimSpace(response.ResultJson) != "100" {
			t.Fatalf("Unexpected result: %s", response.ResultJson)
		}
	})
	t.Run("command with message", func(t *testing.T) {
		config := DefaultConfig()
		config.SkipJSockDVersionCheck = true
		config.NThreads = 1
		//config.Logger = logger
		client, err := InitJSockDClient(config, getJSockDPath(t))
		if err != nil {
			t.Fatal(err)
		}
		defer client.Close()
		if err != nil {
			t.Fatal(err)
		}
		msgCount := 0
		var msgErr error
		response, err := SendRawCommandWithMessageHandler(client, "(m, p) => { JSockD.sendMessage(\"foo\"); return JSockD.sendMessage(\"bar\"); }", "99", func(json string) (string, error) {
			if msgCount == 0 && json != "\"foo\"" {
				msgErr = fmt.Errorf("Unexpected first message: '%s' '%s'", json, "\"foo\"")
			}
			if msgCount == 1 && json != "\"bar\"" {
				msgErr = fmt.Errorf("Unexpected second message: '%s' '%s'", json, "\"bar\"")
			}
			if msgCount > 1 && msgErr == nil {
				msgErr = fmt.Errorf("Unexpected extra message: %s", json)
			}
			msgCount++
			if msgCount == 1 {
				return "\"ack-1\"", nil
			}
			return "\"ack-2\"", nil
		})
		if msgErr != nil {
			t.Fatalf("Message error: %v", msgErr)
		}
		if response.Exception {
			t.Fatalf("Exception: %s", response.ResultJson)
		}
		if response.ResultJson != "\"ack-2\"" {
			t.Fatalf("Unexpected result: %s", response.ResultJson)
		}
	})
	t.Run("bad command", func(t *testing.T) {
		config := DefaultConfig()
		config.SkipJSockDVersionCheck = true
		config.NThreads = 1
		//config.Logger = logger
		client, err := InitJSockDClient(config, getJSockDPath(t))
		if err != nil {
			t.Fatal(err)
		}
		defer client.Close()
		if err != nil {
			t.Fatal(err)
		}
		response, err := SendRawCommand(client, "(m, p) => p.foo()", "99")
		if !response.Exception {
			t.Fatal("Expected exceptional response")
		}
		t.Logf("Got exception: %s", response.ResultJson)
	})
}

func TestSendCommand(t *testing.T) {
	t.Run("good command", func(t *testing.T) {
		config := DefaultConfig()
		config.SkipJSockDVersionCheck = true
		config.NThreads = 1
		//config.Logger = logger
		client, err := InitJSockDClient(config, getJSockDPath(t))
		if err != nil {
			t.Fatal(err)
		}
		defer client.Close()
		if err != nil {
			t.Fatal(err)
		}
		resp, err := SendCommand[int](client, "(m, p) => p+1", 99)
		if err != nil {
			t.Fatal(err)
		}
		if resp.Exception {
			t.Fatal("Unexpected exception")
		}
		if resp.Result != 100 {
			t.Fatalf("Unexpected result: %d", resp.Result)
		}
	})
	t.Run("bad command", func(t *testing.T) {
		config := DefaultConfig()
		config.SkipJSockDVersionCheck = true
		config.NThreads = 1
		//config.Logger = logger
		client, err := InitJSockDClient(config, getJSockDPath(t))
		if err != nil {
			t.Fatal(err)
		}
		defer client.Close()
		if err != nil {
			t.Fatal(err)
		}
		resp, err := SendCommand[int](client, "(m, p) => p.foo()", 99)
		if err != nil {
			t.Fatal(err)
		}
		if !resp.Exception {
			t.Fatal("Expected exceptional response")
		}
	})
}

var jsockdPath string
var jsockdPathMutex sync.Mutex
var jsockdDirPath string

const jsockdReleaseTempl = "https://github.com/addrummond/jsockd/releases/download/vVERSION/jsockd-VERSION-OS-ARCH.tar.gz"

func getJSockDPath(t *testing.T) string {
	if jsockd := os.Getenv("JSOCKD"); jsockd != "" {
		t.Logf("Using jsockd from JSOCKD environment variable: %s", jsockd)
		return jsockd
	}

	jsockdPathMutex.Lock()
	defer jsockdPathMutex.Unlock()
	if jsockdPath != "" {
		t.Logf("Using exising jsockd: %s", jsockdPath)
		return jsockdPath
	}

	t.Logf("Downloading and verifying jsockd: %s", jsockdPath)

	var err error
	jsockdPath, err = downloadAndVerifyJSockD()
	if err != nil {
		t.Fatalf("Failed to get jsockd: %v", err)
	}

	return jsockdPath
}

func TestMain(m *testing.M) {
	code := m.Run()
	if jsockdDirPath != "" {
		os.RemoveAll(jsockdDirPath)
	}
	os.Exit(code)
}

// func logger(timestamp time.Time, level string, message string) {
// 	fmt.Printf("%s [%s] %s\n", timestamp.Format("2006-01-02 15:04:05"), level, message)
// }
