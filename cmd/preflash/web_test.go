package main

import (
	"fmt"
	"net"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

func TestWebRefusesATakenPort(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	require.NoError(t, err)
	defer ln.Close()
	port := ln.Addr().(*net.TCPAddr).Port

	err = webCommand([]string{"--project", "../../examples/demo", "--port", fmt.Sprint(port)})
	require.Error(t, err)
	assert.Contains(t, err.Error(), fmt.Sprintf("port %d is in use", port))
	assert.Contains(t, err.Error(), "--port")
}
