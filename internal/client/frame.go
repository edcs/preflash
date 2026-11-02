package client

import (
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"strings"
)

const (
	bmpHeaderSize = 54
	bmpPixelBytes = 3
)

// Frame is what the panel shows.
type Frame struct {
	Width  int    `json:"width"`
	Height int    `json:"height"`
	Format string `json:"format"`
	Data   []byte `json:"data"`
}

// Save writes the frame as a PNG or a BMP, chosen by the file's extension.
func (f Frame) Save(path string) error {
	if f.Format != "rgb565le" {
		return fmt.Errorf("a frame in %q, which this preflash can't read", f.Format)
	}
	// Divides instead of multiplying, which a bad reply could overflow.
	if f.Width <= 0 || f.Height <= 0 || len(f.Data)%2 != 0 || len(f.Data)/2/f.Width != f.Height ||
		len(f.Data)/2%f.Width != 0 {
		return fmt.Errorf("a %dx%d frame of %d bytes", f.Width, f.Height, len(f.Data))
	}
	var encoded []byte
	var err error
	switch strings.ToLower(filepath.Ext(path)) {
	case ".png":
		encoded, err = f.png()
	case ".bmp":
		encoded = f.bmp()
	default:
		err = fmt.Errorf("%s: a shot is a .png or a .bmp", path)
	}
	if err != nil {
		return err
	}
	return os.WriteFile(path, encoded, 0o644)
}

// pixel widens one RGB565 pixel, repeating the top bits so that full scale is 255.
func (f Frame) pixel(x, y int) (r, g, b uint8) {
	at := (y*f.Width + x) * 2
	v := binary.LittleEndian.Uint16(f.Data[at:])
	red, green, blue := uint8(v>>11)&0x1f, uint8(v>>5)&0x3f, uint8(v)&0x1f
	return red<<3 | red>>2, green<<2 | green>>4, blue<<3 | blue>>2
}

func (f Frame) png() ([]byte, error) {
	img := image.NewNRGBA(image.Rect(0, 0, f.Width, f.Height))
	for y := range f.Height {
		for x := range f.Width {
			r, g, b := f.pixel(x, y)
			img.SetNRGBA(x, y, color.NRGBA{R: r, G: g, B: b, A: 255})
		}
	}
	var out strings.Builder
	if err := png.Encode(&out, img); err != nil {
		return nil, err
	}
	return []byte(out.String()), nil
}

// bmp lays the file out as the runtime's @sim shot does, so the two compare equal.
func (f Frame) bmp() []byte {
	stride := (f.Width*bmpPixelBytes + 3) &^ 3
	size := stride * f.Height
	out := make([]byte, bmpHeaderSize+size)
	out[0], out[1] = 'B', 'M'
	binary.LittleEndian.PutUint32(out[2:], uint32(len(out)))
	binary.LittleEndian.PutUint32(out[10:], bmpHeaderSize)
	binary.LittleEndian.PutUint32(out[14:], 40) // size of the info header
	binary.LittleEndian.PutUint32(out[18:], uint32(f.Width))
	binary.LittleEndian.PutUint32(out[22:], uint32(f.Height))
	binary.LittleEndian.PutUint16(out[26:], 1) // planes
	binary.LittleEndian.PutUint16(out[28:], bmpPixelBytes*8)
	binary.LittleEndian.PutUint32(out[34:], uint32(size))
	for y := range f.Height {
		row := out[bmpHeaderSize+(f.Height-1-y)*stride:]
		for x := range f.Width {
			r, g, b := f.pixel(x, y)
			row[x*bmpPixelBytes], row[x*bmpPixelBytes+1], row[x*bmpPixelBytes+2] = b, g, r
		}
	}
	return out
}
