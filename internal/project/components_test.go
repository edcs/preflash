package project

import (
	"archive/tar"
	"archive/zip"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync/atomic"
	"testing"

	"github.com/stretchr/testify/assert"
	"github.com/stretchr/testify/require"
)

// componentFiles is what the fake registry's lvgl holds.
var componentFiles = map[string]string{
	"CMakeLists.txt": "idf_component_register()\n",
	"lv_version.h":   "#define LVGL_VERSION_MAJOR 9\n#define LVGL_VERSION_MINOR 3\n#define LVGL_VERSION_PATCH 0\n",
	"src/lvgl.c":     "int lvgl;\n",
}

// componentHash works the hash out the component manager's way, by hand, so that the code
// under test has something to disagree with.
func expectedHash(files map[string]string) string {
	sum := sha256.New()
	for _, name := range []string{"CMakeLists.txt", "lv_version.h", "src/lvgl.c"} {
		content, ok := files[name]
		if !ok {
			continue
		}
		file := sha256.Sum256([]byte(content))
		sum.Write([]byte(name))
		sum.Write([]byte(hex.EncodeToString(file[:])))
	}
	return hex.EncodeToString(sum.Sum(nil))
}

func zipOf(t *testing.T, files map[string]string) []byte {
	t.Helper()
	var out bytes.Buffer
	archive := zip.NewWriter(&out)
	for name, content := range files {
		w, err := archive.Create(name)
		require.NoError(t, err)
		_, err = w.Write([]byte(content))
		require.NoError(t, err)
	}
	require.NoError(t, archive.Close())
	return out.Bytes()
}

func tarOf(t *testing.T, files map[string]string) []byte {
	t.Helper()
	var out bytes.Buffer
	gz := gzip.NewWriter(&out)
	archive := tar.NewWriter(gz)
	for name, content := range files {
		require.NoError(t, archive.WriteHeader(&tar.Header{Name: name, Mode: 0o644, Size: int64(len(content))}))
		_, err := archive.Write([]byte(content))
		require.NoError(t, err)
	}
	require.NoError(t, archive.Close())
	require.NoError(t, gz.Close())
	return out.Bytes()
}

// registry serves one archive for lvgl/lvgl 9.3.0, as the component registry does, and
// counts the archives it hands out.
func registry(t *testing.T, name string, archive []byte) (*httptest.Server, *atomic.Int32) {
	t.Helper()
	var downloads atomic.Int32
	var server *httptest.Server
	server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch r.URL.Path {
		case "/api/components/lvgl/lvgl":
			fmt.Fprintf(w, `{"versions":[{"version":"8.0.0","url":"%[1]s/wrong"},{"version":"9.3.0","url":"%[1]s/%[2]s"}]}`, server.URL, name)
		case "/" + name:
			downloads.Add(1)
			w.Write(archive)
		default:
			http.NotFound(w, r)
		}
	}))
	t.Cleanup(server.Close)
	return server, &downloads
}

func lockFor(url, hash string) string {
	return fmt.Sprintf(`dependencies:
  espressif/esp_lvgl_port:
    component_hash: 0000
    source:
      registry_url: %[1]s
      type: service
    version: 2.9.0
  idf:
    source:
      type: idf
    version: 5.4.2
  lvgl/lvgl:
    component_hash: %[2]s
    source:
      registry_url: %[1]s/
      type: service
    version: 9.3.0
target: esp32s3
version: 2.0.0
`, url, hash)
}

// lockedProject is a project with only a lock file, in a home of its own.
func lockedProject(t *testing.T, lock string) (Project, string) {
	t.Helper()
	t.Setenv("HOME", t.TempDir())
	dir := idfProject(t, t.TempDir())
	writeFile(t, filepath.Join(dir, "dependencies.lock"), lock)
	p, err := Find(dir)
	require.NoError(t, err)
	root, err := Root()
	require.NoError(t, err)
	return p, root
}

func TestALockedProjectGetsItsComponentsOutsideItsFolder(t *testing.T) {
	for name, archive := range map[string][]byte{
		"lvgl.zip": zipOf(t, componentFiles),
		"lvgl.tgz": tarOf(t, componentFiles),
	} {
		t.Run(name, func(t *testing.T) {
			server, _ := registry(t, name, archive)
			p, root := lockedProject(t, lockFor(server.URL, expectedHash(componentFiles)))

			var output bytes.Buffer
			dir, err := p.fetchComponents(context.Background(), root, &output)
			require.NoError(t, err)

			assert.Contains(t, output.String(), "lvgl/lvgl 9.3.0")
			assert.Equal(t, "managed_components", filepath.Base(dir))
			assert.FileExists(t, filepath.Join(dir, "lvgl__lvgl", "src", "lvgl.c"))
			assert.NotContains(t, dir, p.Dir)
			assert.NoDirExists(t, filepath.Join(dir, "espressif__esp_lvgl_port"), "the runtime stands in for it")
			assert.NoDirExists(t, filepath.Join(p.Dir, "managed_components"))
		})
	}
}

func TestComponentsAreFetchedOnce(t *testing.T) {
	server, downloads := registry(t, "lvgl.zip", zipOf(t, componentFiles))
	p, root := lockedProject(t, lockFor(server.URL, expectedHash(componentFiles)))

	first, err := p.fetchComponents(context.Background(), root, &bytes.Buffer{})
	require.NoError(t, err)
	second, err := p.fetchComponents(context.Background(), root, &bytes.Buffer{})
	require.NoError(t, err)

	assert.Equal(t, first, second)
	assert.EqualValues(t, 1, downloads.Load())
}

func TestTheManagersOwnFilesStayOutOfTheHash(t *testing.T) {
	files := map[string]string{".component_hash": "x", "CHECKSUMS.json": "{}"}
	for name, content := range componentFiles {
		files[name] = content
	}
	server, _ := registry(t, "lvgl.zip", zipOf(t, files))
	p, root := lockedProject(t, lockFor(server.URL, expectedHash(componentFiles)))

	_, err := p.fetchComponents(context.Background(), root, &bytes.Buffer{})
	require.NoError(t, err)
}

func TestAComponentThatDoesntMatchItsHashIsRefused(t *testing.T) {
	server, _ := registry(t, "lvgl.zip", zipOf(t, map[string]string{"CMakeLists.txt": "something else"}))
	p, root := lockedProject(t, lockFor(server.URL, expectedHash(componentFiles)))

	dir, err := p.fetchComponents(context.Background(), root, &bytes.Buffer{})

	require.ErrorContains(t, err, "lvgl/lvgl 9.3.0")
	require.ErrorContains(t, err, "hash")
	assert.Empty(t, dir)
	again, _ := filepath.Glob(filepath.Join(p.componentStore(), "*", "*", "*"))
	assert.Empty(t, again, "nothing a later build could trust is left behind")
}

func TestAnArchiveCantWriteOutsideItsFolder(t *testing.T) {
	escape := map[string]string{"../../escaped.txt": "x", "CMakeLists.txt": "y"}
	for name, archive := range map[string][]byte{"lvgl.zip": zipOf(t, escape), "lvgl.tgz": tarOf(t, escape)} {
		t.Run(name, func(t *testing.T) {
			server, _ := registry(t, name, archive)
			p, root := lockedProject(t, lockFor(server.URL, expectedHash(componentFiles)))

			_, err := p.fetchComponents(context.Background(), root, &bytes.Buffer{})

			require.ErrorContains(t, err, "escaped.txt")
		})
	}
}

func TestAVersionTheRegistryDoesntHaveIsNamed(t *testing.T) {
	server, _ := registry(t, "lvgl.zip", zipOf(t, componentFiles))
	lock := bytes.ReplaceAll([]byte(lockFor(server.URL, "abc")), []byte("version: 9.3.0"), []byte("version: 9.9.9"))
	p, root := lockedProject(t, string(lock))

	_, err := p.fetchComponents(context.Background(), root, &bytes.Buffer{})

	require.ErrorContains(t, err, "lvgl/lvgl 9.9.9")
}

func TestAProjectsOwnManagedComponentsWin(t *testing.T) {
	server, downloads := registry(t, "lvgl.zip", zipOf(t, componentFiles))
	p, root := lockedProject(t, lockFor(server.URL, expectedHash(componentFiles)))
	require.NoError(t, os.Mkdir(filepath.Join(p.Dir, "managed_components"), 0o755))

	dir, err := p.fetchComponents(context.Background(), root, &bytes.Buffer{})

	require.NoError(t, err)
	assert.Empty(t, dir)
	assert.Zero(t, downloads.Load())
}

func TestAProjectWithNoLockFetchesNothing(t *testing.T) {
	t.Setenv("HOME", t.TempDir())
	p, err := Find(idfProject(t, t.TempDir()))
	require.NoError(t, err)
	root, err := Root()
	require.NoError(t, err)

	dir, err := p.fetchComponents(context.Background(), root, &bytes.Buffer{})

	require.NoError(t, err)
	assert.Empty(t, dir)
}

func TestLVGLVersionComesFromTheLockWithNoManagedFolder(t *testing.T) {
	p, _ := lockedProject(t, lockFor("https://registry.invalid", "abc"))

	assert.Equal(t, "9.3.0", p.LVGLVersion())
}
