// Synthetic tape parts as .tar.bz2: segment k is "segNN." repeated 200+37k times.
// part1 holds 00, 01 and a notes file; part2 holds 02 and 03; part2bad flips
// one bit of 02.
package amixtape

const (
	fixturePart1    = "QlpoOTFBWSZTWbUTDZ4AAyLfgMmUQAH3gAACBSBixd4ACAgwALgMZMTTCaYmAmmAxkxNMJpiYCaYBFITSHqaAANGTTQytthtDSUAmuqoG+7hk8Ww0oYMm8QIB+yDbMBBgFF9MuLAXwqI8qsgD2Ag9ZK+TzcDSGtUUWdRBW/DvG9m0MO1TY0NUS7+Psx9Obm4oijlPsiKOkftXWWz8N2aoDfAKLGjZx0bCP8XckU4UJC1Ew2e"
	fixturePart2    = "QlpoOTFBWSZTWZOaX7IAA9vbpMiAQAH/gAQgYoBeACAAAQAICDAArEJJIG0mgABoUAA0AAAVJRMjQaMjQYkXiUGIwSBrBCU58jDg8JDlyEmAYrk0EFJEh6dVxLlEZrBkp30QgqpIkLjHCzKkQhirFdmtvvwWttrTRq/NYQh5hCGp9gg3pIkK3WFCSBfi7kinChISc0v2QA=="
	fixturePart2Bad = "QlpoOTFBWSZTWbyWKBEAA9vbpMiAQAH/gAQgYoBeACAAAQAICDAArASSQNpMgAAKAAaAAAKkqaekA0ZGgxIvE+GIwSBrBCU58DDg8JDlyEmAYrkwBVSSqqIkPTqsSyiMm4Yqd84IKoSQsYct2NIhDBWC2S6+/kua63GbR+aQhDzCEND7AG1JBFbLNnOE/i7kinChIXksUCI="
)
