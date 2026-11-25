package web

import (
	"io"
	"io/fs"
	"net"
	"net/http"
	"regexp"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestThePageAndItsAssetsAreServed(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	srv := &http.Server{Handler: New(Options{}).mux}
	go srv.Serve(ln)
	t.Cleanup(func() { srv.Close() })
	base := "http://" + ln.Addr().String() + "/"

	get := func(path string) (string, string) {
		res, err := http.Get(base + path)
		require.NoError(t, err)
		defer res.Body.Close()
		require.Equal(t, 200, res.StatusCode, path)
		body, err := io.ReadAll(res.Body)
		require.NoError(t, err)
		return res.Header.Get("Content-Type"), string(body)
	}

	kind, page := get("")
	assert.True(t, strings.HasPrefix(kind, "text/html"), kind)
	for _, asset := range []string{"app.js", "style.css", "vendor/xterm.js", "vendor/xterm.css", "vendor/addon-fit.js"} {
		assert.Contains(t, page, `"`+asset+`"`)
		kind, _ := get(asset)
		if strings.HasSuffix(asset, ".css") {
			assert.True(t, strings.HasPrefix(kind, "text/css"), "%s is %s", asset, kind)
		} else {
			assert.Contains(t, kind, "javascript", "%s is %s", asset, kind)
		}
	}

	// Nothing reaches off the machine: every asset is served from here.
	static, err := fs.Sub(staticFiles, "static")
	require.NoError(t, err)
	remote := regexp.MustCompile(`https?://`)
	require.NoError(t, fs.WalkDir(static, ".", func(path string, d fs.DirEntry, err error) error {
		require.NoError(t, err)
		if d.IsDir() || strings.Contains(path, "LICENSE") {
			return nil
		}
		data, err := fs.ReadFile(static, path)
		require.NoError(t, err)
		assert.False(t, remote.MatchString(withoutComments(string(data))), "%s links off the machine", path)
		return nil
	}))
}

var comments = regexp.MustCompile(`(?s)/\*.*?\*/|<!--.*?-->|(^|[^:"'\\])//[^\n]*`)

// withoutComments drops /* */, <!-- --> and // comments, roughly: a // after a colon or quote
// is kept, so a URL in a string still counts.
func withoutComments(s string) string {
	return comments.ReplaceAllString(s, "$1")
}
