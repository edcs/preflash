#pragma once

#include <iosfwd>

namespace preflash {

// Sends each transaction on in through the I2C buses and writes what came back to out, one JSON
// line for each. A transaction is {"bus", "addr", "write": hex, "read_len"}, and an answer is
// {"acked", "read": hex}, with "refused" added if the model refused it. A refusal doesn't stop the
// replay, because it's what a check wants to hear about. Throws on a transaction it can't read.
void replay(std::istream& in, std::ostream& out);

}  // namespace preflash
