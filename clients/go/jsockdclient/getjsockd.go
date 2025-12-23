package jsockdclient

import (
	"archive/tar"
	"bufio"
	"bytes"
	"compress/gzip"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"net/http"
	"os"
	"path"
	"runtime"
	"strings"
)

const jsockdReleaseUrlTempl = "https://github.com/addrummond/jsockd/releases/download/vVERSION/jsockd-VERSION-OS-ARCH.tar.gz"
const jsockdSignatureUrlTempl = "https://github.com/addrummond/jsockd/releases/download/vVERSION/ed25519_signatures.txt"
const jsockdBinaryPublicKey = "b136fca8fbfc42fe6dc95dedd035b0b50ad93b6a5d6fcaf8fc0552e9d29ee406"

// If an errors.Is(err, ErrDownload) is true then the function that returned err may reasonably be retried.
var ErrDownload = errors.New("failed to download file")

func downloadAndVerifyJSockD() (string, error) {
	jsockdPath := ""

	osName := runtime.GOOS
	if osName == "darwin" {
		osName = "macos"
	}
	arch := runtime.GOARCH
	if arch == "amd64" {
		arch = "x86_64"
	}

	url := jsockdReleaseUrlTempl
	url = strings.ReplaceAll(url, "VERSION", JSockDVersion)
	url = strings.ReplaceAll(url, "OS", osName)
	url = strings.ReplaceAll(url, "ARCH", arch)

	sigUrl := jsockdSignatureUrlTempl
	sigUrl = strings.ReplaceAll(sigUrl, "VERSION", JSockDVersion)

	// Download the signatures file
	sigResp, err := http.Get(sigUrl)
	if err != nil {
		return jsockdPath, fmt.Errorf("%w: %s: %w", ErrDownload, sigUrl, err)
	}
	defer sigResp.Body.Close()
	if sigResp.StatusCode != 200 {
		return jsockdPath, fmt.Errorf("%w: unexpected HTTP status %s for %s", ErrDownload, sigResp.Status, sigUrl)
	}

	// Parse signatures file to find the signature for our archive
	archiveFilename := path.Base(url)
	var signature []byte
	scanner := bufio.NewScanner(sigResp.Body)
	for scanner.Scan() {
		line := scanner.Text()
		parts := strings.Split(line, "\t")
		if len(parts) != 2 {
			continue
		}
		if parts[1] == archiveFilename {
			signature, err = base64.StdEncoding.DecodeString(parts[0])
			if err != nil {
				return jsockdPath, fmt.Errorf("decode signature: %w", err)
			}
			break
		}
	}
	if err := scanner.Err(); err != nil {
		return jsockdPath, fmt.Errorf("read signatures file: %w", err)
	}
	if signature == nil {
		return jsockdPath, fmt.Errorf("signature not found for %s", archiveFilename)
	}

	// Decode the public key
	publicKey, err := hex.DecodeString(jsockdBinaryPublicKey)
	if err != nil || len(publicKey) != ed25519.PublicKeySize {
		panic("Duff public key in clients/go/jsockdclient/getjsockd.go")
	}

	// Download the release tar.gz into memory
	resp, err := http.Get(url)
	if err != nil {
		return jsockdPath, fmt.Errorf("download %s: %w", url, err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return jsockdPath, fmt.Errorf("%w: unexpected HTTP status %s for %s", ErrDownload, resp.Status, url)
	}

	archiveData, err := io.ReadAll(resp.Body)
	if err != nil {
		return jsockdPath, fmt.Errorf("read archive: %w", err)
	}

	// Verify the signature
	if !ed25519.Verify(ed25519.PublicKey(publicKey), archiveData, signature) {
		return jsockdPath, errors.New("signature verification failed")
	}

	// Create a temp dir to extract the binary
	jsockdDirPath, err := os.MkdirTemp("", "jsockd-*")
	if err != nil {
		return jsockdPath, fmt.Errorf("mktemp: %w", err)
	}

	// Unpack gzip -> tar, extract jsockd binary
	gr, err := gzip.NewReader(bytes.NewReader(archiveData))
	if err != nil {
		return jsockdPath, fmt.Errorf("gzip open: %w", err)
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
		return jsockdPath, errors.New("jsockd binary not found in archive")
	}

	return jsockdPath, nil
}
