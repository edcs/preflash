package client

import "fmt"

// TraceRecord is one thing firmware did to the hardware. A gap in Seq means records were
// lost to a reader that fell behind.
type TraceRecord struct {
	Stream   string `json:"stream"` // "i2c", "gpio", "task", "nvs" or "net"
	Seq      uint64 `json:"seq"`
	Us       int64  `json:"us"`      // since boot
	Subject  string `json:"subject"` // an address, a pin, a task, a key, or "wifi" or "http"
	Op       string `json:"op"`
	Detail   string `json:"detail"`
	Injected bool   `json:"injected"` // a forced failure caused this
	Bus      int    `json:"bus"`      // i2c only, like the two below
	Address  int    `json:"address"`
	Device   string `json:"device"` // the board's chip at that address, if any
}

// Time is when the record happened, like the mockup's 04:11.204.
func (r TraceRecord) Time() string {
	ms := r.Us / 1000
	seconds := ms / 1000
	if hours := seconds / 3600; hours > 0 {
		return fmt.Sprintf("%d:%02d:%02d.%03d", hours, seconds/60%60, seconds%60, ms%1000)
	}
	return fmt.Sprintf("%02d:%02d.%03d", seconds/60, seconds%60, ms%1000)
}

// Task is a task firmware started. Stack is what firmware declared, not what it uses.
type Task struct {
	Name  string `json:"name"`
	State string `json:"state"`
	Stack int    `json:"stack"`
}

// Namespace is one NVS namespace as committed.
type Namespace struct {
	Name string   `json:"name"`
	Keys []NVSKey `json:"keys"`
}

// NVSKey is a key and the size of its value. Values stay in the app.
type NVSKey struct {
	Key  string `json:"key"`
	Size int    `json:"size"`
}

// HardwareOption is one setting of a hardware item.
type HardwareOption struct {
	Value string `json:"value"`
	Label string `json:"label"`
}

// HardwareItem is one thing that can be changed on the board.
type HardwareItem struct {
	Name    string           `json:"name"`
	Title   string           `json:"label"`
	Group   string           `json:"group"`
	Applies string           `json:"applies"` // "now" or "boot"
	Value   string           `json:"value"`   // in force now
	Next    string           `json:"next"`    // what the next boot gets
	Options []HardwareOption `json:"options"`
}

// Label gives the words for one of the item's values.
func (h HardwareItem) Label(value string) string {
	for _, option := range h.Options {
		if option.Value == value {
			return option.Label
		}
	}
	return value
}

// Waiting says the next boot brings a change.
func (h HardwareItem) Waiting() bool { return h.Next != h.Value }

// Tasks lists the tasks firmware has running.
func (c *Client) Tasks() ([]Task, error) {
	var reply struct {
		Tasks []Task `json:"tasks"`
	}
	err := c.Call("tasks", nil, &reply)
	return reply.Tasks, err
}

// NVS lists what's committed to NVS.
func (c *Client) NVS() ([]Namespace, error) {
	var reply struct {
		Namespaces []Namespace `json:"namespaces"`
	}
	err := c.Call("nvs", nil, &reply)
	return reply.Namespaces, err
}

type hardwareReply struct {
	Items []HardwareItem `json:"items"`
}

// Hardware reads the board's physical state and forced failures.
func (c *Client) Hardware() ([]HardwareItem, error) {
	var reply hardwareReply
	err := c.Call("hardware", nil, &reply)
	return reply.Items, err
}

// SetHardware changes one item and gives back all of them as they now stand.
func (c *Client) SetHardware(name, value string) ([]HardwareItem, error) {
	var reply hardwareReply
	err := c.Call("hardware_set", map[string]any{"name": name, "value": value}, &reply)
	return reply.Items, err
}

// SaveTrace writes what the app's ring holds, on the app's side, and says where and how many
// records. With no path, the app picks one in its state folder.
func (c *Client) SaveTrace(path string) (string, int, error) {
	var reply struct {
		Path    string `json:"path"`
		Records int    `json:"records"`
	}
	var fields map[string]any
	if path != "" {
		fields = map[string]any{"path": path}
	}
	err := c.Call("trace_save", fields, &reply)
	return reply.Path, reply.Records, err
}
