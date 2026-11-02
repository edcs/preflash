package check

// noRegister is a byte that isn't tied to a register, such as a reply to a command.
const noRegister = -1

const pcf85063Registers = 0x12

// expected is a run of registers that can't match a model, and why.
type expected struct {
	first, last int
	reason      string
}

// Each chip's registers that hold the moment or the room rather than how the chip behaves. The
// model has its own clock, stands still, and has the power it's told it has.
var expectedRegisters = map[string][]expected{
	"pcf85063": {{0x04, 0x0a, "time"}},
	"qmi8658":  {{0x35, 0x3a, "sensor noise"}},
	"axp2101":  {{0x00, 0x01, "power state"}, {0xa4, 0xa4, "power state"}},
	"gt911":    {{0x814e, 0x8157, "sensor noise"}},
}

// expectedReason says why a difference at this register is no surprise, or nothing if it is one.
func expectedReason(chip string, register int) string {
	for _, run := range expectedRegisters[chip] {
		if register >= run.first && register <= run.last {
			return run.reason
		}
	}
	return ""
}

// registerAt finds the register that the byte at offset came from, as each chip moves through its
// registers during a burst.
func registerAt(chip string, write []byte, offset int) int {
	switch {
	case chip == "axs15231b", len(write) == 0:
		return noRegister
	case chip == "gt911":
		if len(write) < 2 {
			return noRegister
		}
		return int(write[0])<<8 + int(write[1]) + offset
	case chip == "tca9554":
		return int(write[0])
	case chip == "pcf85063":
		return (int(write[0]) + offset) % pcf85063Registers
	default:
		return (int(write[0]) + offset) & 0xff
	}
}
