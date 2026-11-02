package project

import (
	"archive/tar"
	"archive/zip"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"os"
	"path"
	"path/filepath"
	"regexp"
	"slices"
	"strings"

	"gopkg.in/yaml.v3"
)

const (
	lockName    = "dependencies.lock"
	managedName = "managed_components"
)

// component is one download a project's lock file pins, by version and hash.
type component struct {
	Name     string
	Version  string
	Hash     string
	Registry string
}

// folder is the component's name in managed_components/.
func (c component) folder() string { return strings.ReplaceAll(c.Name, "/", "__") }

func (c component) String() string { return c.Name + " " + c.Version }

// lockedComponents reads what the project's lock pins from the registry, in name order. A
// project with no lock has none.
func (p Project) lockedComponents() ([]component, error) {
	data, err := os.ReadFile(filepath.Join(p.Dir, lockName))
	if errors.Is(err, fs.ErrNotExist) {
		return nil, nil
	}
	if err != nil {
		return nil, err
	}
	var lock struct {
		Dependencies map[string]struct {
			Hash    string `yaml:"component_hash"`
			Version string `yaml:"version"`
			Source  struct {
				Type     string `yaml:"type"`
				Registry string `yaml:"registry_url"`
			} `yaml:"source"`
		} `yaml:"dependencies"`
	}
	if err := yaml.Unmarshal(data, &lock); err != nil {
		return nil, fmt.Errorf("%s: %w", lockName, err)
	}
	var out []component
	for name, dep := range lock.Dependencies {
		if dep.Source.Type == "service" {
			out = append(out, component{name, dep.Version, dep.Hash, strings.TrimRight(dep.Source.Registry, "/")})
		}
	}
	slices.SortFunc(out, func(a, b component) int { return strings.Compare(a.Name, b.Name) })
	return out, nil
}

var replacedPattern = regexp.MustCompile(`"([^"]+)"`)

// replacedComponents reads the list project.cmake leaves out of a build, which has the one
// copy of it.
func replacedComponents(root string) ([]*regexp.Regexp, error) {
	data, err := os.ReadFile(filepath.Join(root, "runtime", "idf", "tools", "cmake", "replaced.cmake"))
	if err != nil {
		return nil, err
	}
	var out []*regexp.Regexp
	for _, line := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(strings.TrimSpace(line), "#") {
			continue
		}
		for _, match := range replacedPattern.FindAllStringSubmatch(line, -1) {
			parts := strings.Split(match[1], "*")
			for i, part := range parts {
				parts[i] = regexp.QuoteMeta(part)
			}
			out = append(out, regexp.MustCompile("^"+strings.Join(parts, ".*")+"$"))
		}
	}
	return out, nil
}

// componentStore holds fetched components, shared by every checkout that pins the same ones.
func (p Project) componentStore() string { return filepath.Join(preflashHome(), "components") }

// fetchComponents gets what the project's lock pins and has no stand-in for, so that a clean
// clone runs with no ESP-IDF installed, and says where they are. The project's own folder
// never changes. It answers "" when there's nothing to fetch, because the project has a
// managed_components/ of its own or no lock. An idf.py in the checkout makes the first of
// those true, and from then on that folder is what's built, as it is on the device.
func (p Project) fetchComponents(ctx context.Context, root string, output io.Writer) (string, error) {
	if info, err := os.Stat(filepath.Join(p.Dir, managedName)); err == nil && info.IsDir() {
		return "", nil
	}
	locked, err := p.lockedComponents()
	if err != nil {
		return "", err
	}
	replaced, err := replacedComponents(root)
	if err != nil {
		return "", err
	}
	needed := slices.DeleteFunc(locked, func(c component) bool {
		return slices.ContainsFunc(replaced, func(pattern *regexp.Regexp) bool { return pattern.MatchString(c.folder()) })
	})
	if len(needed) == 0 {
		return "", nil
	}

	sum := sha256.New()
	for _, c := range needed {
		fmt.Fprintln(sum, c.Name, c.Version, c.Hash)
	}
	final := filepath.Join(p.componentStore(), hex.EncodeToString(sum.Sum(nil))[:16])
	dir := filepath.Join(final, managedName)
	if _, err := os.Stat(dir); err == nil {
		return dir, nil
	}

	if err := os.MkdirAll(p.componentStore(), 0o755); err != nil {
		return "", err
	}
	scratch, err := os.MkdirTemp(p.componentStore(), "fetch-")
	if err != nil {
		return "", err
	}
	defer os.RemoveAll(scratch)
	for _, c := range needed {
		fmt.Fprintf(output, "preflash: fetching %s\n", c)
		if err := c.fetch(ctx, filepath.Join(scratch, managedName, c.folder())); err != nil {
			return "", fmt.Errorf("fetching %s: %w", c, err)
		}
	}
	// A whole folder or none, so a build that was stopped leaves nothing to trust. Another
	// build getting there first is as good as this one.
	if err := os.Rename(scratch, final); err != nil {
		if _, statErr := os.Stat(dir); statErr != nil {
			return "", err
		}
	}
	return dir, nil
}

// fetch unpacks the component into to, which it makes, and checks it against the lock.
func (c component) fetch(ctx context.Context, to string) error {
	url, err := c.archiveURL(ctx)
	if err != nil {
		return err
	}
	download, err := os.CreateTemp(filepath.Dir(filepath.Dir(to)), "archive-")
	if err != nil {
		return err
	}
	defer os.Remove(download.Name())
	defer download.Close()
	if err := get(ctx, url, func(body io.Reader) error {
		_, err := io.Copy(download, body)
		return err
	}); err != nil {
		return err
	}
	if err := unpack(download, url, to); err != nil {
		return err
	}
	got, err := componentHash(to)
	if err != nil {
		return err
	}
	if got != c.Hash {
		return fmt.Errorf("its hash is %s, and %s pins %s", got, lockName, c.Hash)
	}
	return nil
}

// archiveURL asks the registry where this version lives. The lock doesn't say, and doesn't
// need to: the hash is what's pinned.
func (c component) archiveURL(ctx context.Context) (string, error) {
	var listing struct {
		Versions []struct {
			Version string `json:"version"`
			URL     string `json:"url"`
		} `json:"versions"`
	}
	err := get(ctx, c.Registry+"/api/components/"+c.Name, func(body io.Reader) error {
		return json.NewDecoder(body).Decode(&listing)
	})
	if err != nil {
		return "", err
	}
	for _, version := range listing.Versions {
		if version.Version == c.Version {
			return version.URL, nil
		}
	}
	return "", errors.New("the registry doesn't list it")
}

func get(ctx context.Context, url string, read func(io.Reader) error) error {
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return err
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		return fmt.Errorf("%s: %s", url, response.Status)
	}
	return read(response.Body)
}

// unpack reads the registry's zip or gzipped tar. Its entries are checked by unpackFile, so a
// bad archive can't write outside to.
func unpack(archive *os.File, url, to string) error {
	info, err := archive.Stat()
	if err != nil {
		return err
	}
	if path.Ext(url) == ".zip" {
		return unzip(archive, info.Size(), to)
	}
	if _, err := archive.Seek(0, io.SeekStart); err != nil {
		return err
	}
	return untar(archive, to)
}

func unzip(archive io.ReaderAt, size int64, to string) error {
	files, err := zip.NewReader(archive, size)
	if err != nil {
		return err
	}
	for _, file := range files.File {
		if err := unzipFile(file, to); err != nil {
			return err
		}
	}
	return nil
}

func unzipFile(file *zip.File, to string) error {
	if file.Mode().IsDir() {
		return nil
	}
	body, err := file.Open()
	if err != nil {
		return err
	}
	defer body.Close()
	return unpackFile(to, file.Name, file.Mode(), body)
}

func untar(archive io.Reader, to string) error {
	gz, err := gzip.NewReader(archive)
	if err != nil {
		return err
	}
	files := tar.NewReader(gz)
	for {
		header, err := files.Next()
		if errors.Is(err, io.EOF) {
			return nil
		}
		if err != nil {
			return err
		}
		if header.FileInfo().IsDir() {
			continue
		}
		if err := unpackFile(to, header.Name, header.FileInfo().Mode(), files); err != nil {
			return err
		}
	}
}

// unpackFile writes one entry. Only plain files come out: a component has no use for a link,
// and the hash couldn't vouch for what it points at.
func unpackFile(to, name string, mode fs.FileMode, body io.Reader) error {
	if !filepath.IsLocal(name) {
		return fmt.Errorf("the archive holds a path outside it: %s", name)
	}
	if !mode.IsRegular() {
		return fmt.Errorf("the archive holds something that isn't a file: %s", name)
	}
	target := filepath.Join(to, filepath.FromSlash(name))
	if err := os.MkdirAll(filepath.Dir(target), 0o755); err != nil {
		return err
	}
	out, err := os.OpenFile(target, os.O_CREATE|os.O_WRONLY|os.O_EXCL, 0o644)
	if err != nil {
		return err
	}
	_, err = io.Copy(out, body)
	if closed := out.Close(); err == nil {
		err = closed
	}
	return err
}

// componentHash is the sum the component manager writes in a lock file: the hash of each
// file's path and the hex hash of its content, in path order. The two names left out are
// what the manager itself adds to a folder.
func componentHash(dir string) (string, error) {
	var names []string
	err := filepath.WalkDir(dir, func(file string, entry fs.DirEntry, err error) error {
		if err != nil || entry.IsDir() {
			return err
		}
		relative, err := filepath.Rel(dir, file)
		if err != nil {
			return err
		}
		if name := filepath.ToSlash(relative); name != ".component_hash" && name != "CHECKSUMS.json" {
			names = append(names, name)
		}
		return nil
	})
	if err != nil {
		return "", err
	}
	slices.Sort(names)
	sum := sha256.New()
	for _, name := range names {
		content, err := fileHash(filepath.Join(dir, filepath.FromSlash(name)))
		if err != nil {
			return "", err
		}
		fmt.Fprintf(sum, "%s%s", name, content)
	}
	return hex.EncodeToString(sum.Sum(nil)), nil
}

func fileHash(file string) (string, error) {
	in, err := os.Open(file)
	if err != nil {
		return "", err
	}
	defer in.Close()
	sum := sha256.New()
	if _, err := io.Copy(sum, in); err != nil {
		return "", err
	}
	return hex.EncodeToString(sum.Sum(nil)), nil
}
