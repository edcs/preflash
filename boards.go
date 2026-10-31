// Package preflash carries the stock board files inside the binary.
package preflash

import "embed"

// Boards holds the stock board files, under the boards/ folder.
//
//go:embed boards/*.yaml
var Boards embed.FS
