package web

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/stretchr/testify/assert"
)

func TestGuard(t *testing.T) {
	for name, test := range map[string]struct {
		method, path, host, origin string
		upgrade                    bool
		allow                      []string
		status                     int
		body                       []string
	}{
		"localhost":               {"GET", "/", "localhost:7480", "", false, nil, 200, nil},
		"127.0.0.1":               {"GET", "/", "127.0.0.1:9", "", false, nil, 200, nil},
		"ipv6 loopback":           {"GET", "/", "[::1]:7480", "", false, nil, 200, nil},
		"loopback any port":       {"GET", "/device", "localhost:51234", "http://localhost:51234", true, nil, 200, nil},
		"upgrade, foreign origin": {"GET", "/device", "localhost:7480", "https://evil.example", true, nil, 403, nil},
		"upgrade, no origin":      {"GET", "/device", "localhost:7480", "", true, nil, 403, nil},
		"post, same origin":       {"POST", "/x", "localhost:7480", "http://localhost:7480", false, nil, 200, nil},
		"post, no origin":         {"POST", "/x", "localhost:7480", "", false, nil, 403, nil},
		"foreign host":            {"GET", "/", "evil.example", "", false, nil, 403, []string{"evil.example", "--allow-host"}},
		"allowed host":            {"GET", "/", "preflash.test", "", false, []string{"preflash.test"}, 200, nil},
		"allowed host with port":  {"GET", "/", "preflash.test:8443", "", false, []string{"preflash.test"}, 200, nil},
	} {
		t.Run(name, func(t *testing.T) {
			r := httptest.NewRequest(test.method, test.path, nil)
			r.Host = test.host
			if test.origin != "" {
				r.Header.Set("Origin", test.origin)
			}
			if test.upgrade {
				r.Header.Set("Connection", "Upgrade")
				r.Header.Set("Upgrade", "websocket")
			}
			w := httptest.NewRecorder()
			guard(test.allow, http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
				w.WriteHeader(200)
			})).ServeHTTP(w, r)
			assert.Equal(t, test.status, w.Code)
			for _, want := range test.body {
				assert.True(t, strings.Contains(w.Body.String(), want), "body %q lacks %q", w.Body.String(), want)
			}
		})
	}
}
