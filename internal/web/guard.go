package web

import (
	"fmt"
	"net"
	"net/http"
	"net/url"
	"strings"
)

// guard keeps other sites and other machines away from the simulated device and the
// terminal app. A page on another origin can reach localhost, so every request names a
// host we expect, and anything that changes state or opens a socket came from our own page.
func guard(allow []string, next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !hostAllowed(r.Host, allow) {
			refuse(w, r.Host)
			return
		}
		if changesSomething(r) && !sameOrigin(r) {
			refuse(w, r.Host)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func changesSomething(r *http.Request) bool {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		return true
	}
	return strings.EqualFold(r.Header.Get("Upgrade"), "websocket")
}

// hostAllowed takes loopback on any port, since Nearhost forwards from another one.
func hostAllowed(host string, allow []string) bool {
	name := hostName(host)
	switch name {
	case "localhost", "127.0.0.1", "::1":
		return true
	}
	for _, a := range allow {
		if strings.EqualFold(name, a) {
			return true
		}
	}
	return false
}

func hostName(host string) string {
	if name, _, err := net.SplitHostPort(host); err == nil {
		return name
	}
	return strings.Trim(host, "[]")
}

func sameOrigin(r *http.Request) bool {
	origin := r.Header.Get("Origin")
	if origin == "" {
		return false
	}
	u, err := url.Parse(origin)
	return err == nil && strings.EqualFold(u.Host, r.Host)
}

func refuse(w http.ResponseWriter, host string) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.WriteHeader(http.StatusForbidden)
	fmt.Fprintf(w, "Preflash refused this request for %q.\nTo serve it under that name, start preflash web with --allow-host.\n", host)
}
