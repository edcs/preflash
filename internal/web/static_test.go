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
		text := withoutComments(string(data))
		for _, words := range harmlessURLs[path] {
			text = strings.ReplaceAll(text, words, "")
		}
		assert.False(t, remote.MatchString(text), "%s links off the machine", path)
		return nil
	}))
}

// harmlessURLs are addresses in vendored code that nothing loads: text in an error message.
var harmlessURLs = map[string][]string{
	"vendor/opus-decoder.min.js": {"https://github.com/eshaz/simple-yenc"},
}

var comments = regexp.MustCompile(`(?s)/\*.*?\*/|<!--.*?-->|(^|[^:"'\\])//[^\n]*`)

// withoutComments drops /* */, <!-- --> and // comments, roughly: a // after a colon or quote
// is kept, so a URL in a string still counts.
func withoutComments(s string) string {
	return comments.ReplaceAllString(s, "$1")
}

// The zoom controls and the divider are real controls, reachable from the keyboard.
func TestThePageHasZoomControlsAndADivider(t *testing.T) {
	page, err := fs.ReadFile(staticFiles, "static/index.html")
	require.NoError(t, err)
	for _, control := range []string{
		`<button type="button" id="zoom-out"`,
		`<button type="button" id="zoom-in"`,
		`<button type="button" id="zoom-fit"`,
		`id="divider" class="divider" role="separator" tabindex="0"`,
	} {
		assert.Contains(t, string(page), control)
	}
}

// The speaker button and the scripts that play the device's sound are on the page, and served
// as scripts. The worklet isn't in index.html: audio.js loads it into the AudioContext.
func TestThePageHasASpeakerButtonAndItsScripts(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	srv := &http.Server{Handler: New(Options{}).mux}
	go srv.Serve(ln)
	t.Cleanup(func() { srv.Close() })
	base := "http://" + ln.Addr().String() + "/"

	page, err := fs.ReadFile(staticFiles, "static/index.html")
	require.NoError(t, err)
	assert.Contains(t, string(page), `<button type="button" id="speaker"`)
	for _, script := range []string{"vendor/opus-decoder.min.js", "audio.js"} {
		assert.Contains(t, string(page), `<script src="`+script+`"`)
	}
	audio, err := fs.ReadFile(staticFiles, "static/audio.js")
	require.NoError(t, err)
	assert.Contains(t, string(audio), `addModule('audio-worklet.js')`)

	for _, script := range []string{"audio.js", "audio-worklet.js", "vendor/opus-decoder.min.js"} {
		res, err := http.Get(base + script)
		require.NoError(t, err)
		res.Body.Close()
		require.Equal(t, 200, res.StatusCode, script)
		assert.Contains(t, res.Header.Get("Content-Type"), "javascript", script)
	}
	_, err = fs.ReadFile(staticFiles, "static/vendor/LICENSE.opus-decoder")
	assert.NoError(t, err, "the decoder's licence is vendored with it")
}
