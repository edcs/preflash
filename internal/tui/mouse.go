package tui

import (
	"fmt"
	"slices"

	tea "charm.land/bubbletea/v2"
)

// zone is a part of the screen a click acts on, in cells, with x1 exclusive.
type zone struct {
	id        string
	x0, x1, y int
	act       func() tea.Cmd
}

// zones lists what's clickable on the screen as it's drawn now. They come from the same
// layout the view uses, so the two can't drift apart.
func (m *Model) zones() []zone {
	if m.width < minWidth || m.height < minHeight {
		return nil
	}
	zones := m.footerZones()
	if m.picking {
		return append(zones, m.boardZones()...)
	}
	if m.panel != nil {
		return zones
	}
	if m.debug != nil {
		return append(zones, m.debugZones()...)
	}
	if m.crash != nil {
		return zones
	}
	top := m.height - 2 - len(m.chipRows())
	for _, c := range m.chipLayout() {
		zones = append(zones, zone{
			id: fmt.Sprintf("chip %d", c.index),
			x0: c.x0, x1: c.x1, y: top + c.row,
			act: func() tea.Cmd {
				m.useShortcut(c.index)
				return nil
			},
		})
	}
	return zones
}

func (m *Model) footerZones() []zone {
	entries, right := m.footerEntries()
	var zones []zone
	x := sidePad
	y := m.height - 1
	for _, entry := range entries {
		width := entryWidth(entry)
		if act := entry.act; act != nil {
			zones = append(zones, zone{
				id: "footer " + entry.key,
				x0: x, x1: x + width, y: y,
				act: func() tea.Cmd {
					act()
					return nil
				},
			})
		}
		if cmd := entry.cmd; cmd != nil {
			zones = append(zones, zone{id: "footer " + entry.key, x0: x, x1: x + width, y: y, act: cmd})
		}
		x += width + len(footerGap)
	}
	if right.key == "" {
		return zones
	}
	return append(zones, zone{
		id: "footer quit",
		x0: m.width - sidePad - entryWidth(right), x1: m.width - sidePad, y: y,
		act: func() tea.Cmd { return tea.Quit },
	})
}

func (m *Model) zoneAt(x, y int) zone {
	zones := m.zones()
	at := slices.IndexFunc(zones, func(z zone) bool { return y == z.y && x >= z.x0 && x < z.x1 })
	if at < 0 {
		return zone{}
	}
	return zones[at]
}
