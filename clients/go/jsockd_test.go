package jsockdclient

import (
	"archive/tar"
	"compress/gzip"
	"fmt"
	"io"
	"net/http"
	"os"
	"path"
	"runtime"
	"strings"
	"sync"
	"testing"
)

func TestSendRawCommand(t *testing.T) {
	t.Run("good command", func(t *testing.T) {
		config := DefaultConfig()
		config.SkipJSockDVersionCheck = true
		client, err := InitJSockDClient(config, getJSockDPath(t), []string{"/tmp/jsockd.sock"})
		defer client.Close()
		if err != nil {
			t.Fatal(err)
		}
		response, err := SendRawCommand(client, "(m, p) => p+1", "99", nil)
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
		client, err := InitJSockDClient(config, getJSockDPath(t), []string{"/tmp/jsockd.sock"})
		defer client.Close()
		if err != nil {
			t.Fatal(err)
		}
		msgCount := 0
		response, err := SendRawCommand(client, "(m, p) => { JSockD.sendMessage(\"foo\"); return JSockD.sendMessage(\"bar\"); }", "99", func(json string) string {
			if msgCount == 0 && json != "\"foo\"" {
				t.Fatalf("Unexpected first message: %s", json)
			}
			if msgCount == 1 && json != "\"bar\"" {
				t.Fatalf("Unexpected second message: %s", json)
			}
			if msgCount > 1 {
				t.Fatalf("Unexpected extra message: %s", json)
			}
			msgCount++
			if msgCount == 1 {
				return "\"ack-1\""
			}
			return "\"ack-2\""
		})
		if response.Exception {
			t.Fatalf("Exception: %s", response.ResultJson)
		}
		if strings.TrimSpace(response.ResultJson) != "\"ack2\"" {
			t.Fatalf("Unexpected result: %s", response.ResultJson)
		}
	})
	t.Run("bad command", func(t *testing.T) {
		config := DefaultConfig()
		config.SkipJSockDVersionCheck = true
		client, err := InitJSockDClient(config, getJSockDPath(t), []string{"/tmp/jsockd.sock"})
		defer client.Close()
		if err != nil {
			t.Fatal(err)
		}
		response, err := SendRawCommand(client, "(m, p) => p.foo()", "99", nil)
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
		client, err := InitJSockDClient(config, getJSockDPath(t), []string{"/tmp/jsockd.sock"})
		defer client.Close()
		if err != nil {
			t.Fatal(err)
		}
		resp, err := SendCommand[int](client, "(m, p) => p+1", 99, nil)
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
		client, err := InitJSockDClient(config, getJSockDPath(t), []string{"/tmp/jsockd.sock"})
		defer client.Close()
		if err != nil {
			t.Fatal(err)
		}
		resp, err := SendCommand[int](client, "(m, p) => p.foo()", 99, nil)
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
	if ws := os.Getenv("GITHUB_WORKSPACE"); ws != "" {
		jsockdPath = path.Join(ws, "jsockd_server", "build_Release", "jsockd")
		t.Logf("In CI; using jsockd at: %s", jsockdPath)
		return jsockdPath
	}

	osName := runtime.GOOS
	if osName == "darwin" {
		osName = "macos"
	}
	arch := runtime.GOARCH
	if arch == "amd64" {
		arch = "x86_64"
	}

	url := jsockdReleaseTempl
	url = strings.ReplaceAll(url, "VERSION", JSockDVersion)
	url = strings.ReplaceAll(url, "OS", osName)
	url = strings.ReplaceAll(url, "ARCH", arch)

	// Download the release tar.gz
	t.Logf("Downloading jsockd release from %s", url)
	resp, err := http.Get(url)
	if err != nil {
		panic(fmt.Errorf("download %s: %w", url, err))
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		panic(fmt.Errorf("unexpected HTTP status %s for %s", resp.Status, url))
	}

	// Create a temp dir to extract the binary
	jsockdDirPath, err = os.MkdirTemp("", "jsockd-*")
	if err != nil {
		panic(fmt.Errorf("mktemp: %w", err))
	}

	// Unpack gzip -> tar, extract jsockd binary
	gr, err := gzip.NewReader(resp.Body)
	if err != nil {
		panic(fmt.Errorf("gzip open: %w", err))
	}
	defer gr.Close()
	tr := tar.NewReader(gr)

	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			panic(fmt.Errorf("tar read: %w", err))
		}
		if hdr.Typeflag != tar.TypeReg {
			continue
		}
		name := path.Base(hdr.Name)
		if name == "jsockd" {
			out := path.Join(jsockdDirPath, "jsockd")
			f, err := os.OpenFile(out, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o755)
			if err != nil {
				panic(fmt.Errorf("create %s: %w", out, err))
			}
			if _, err := io.Copy(f, tr); err != nil {
				_ = f.Close()
				panic(fmt.Errorf("extract %s: %w", out, err))
			}
			if err := f.Close(); err != nil {
				panic(fmt.Errorf("close %s: %w", out, err))
			}
			jsockdPath = out
			break
		}
	}

	if jsockdPath == "" {
		panic("jsockd binary not found in archive")
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
