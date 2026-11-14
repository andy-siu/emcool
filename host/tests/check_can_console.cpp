// The pure parts of the serial console's manual CAN commands (emcool/can/console.hpp): parsing a
// typed can-send frame and can-dump's id filter, and formatting a received frame.
//
//   check_can_console

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "check.hpp"
#include "emcool/can/console.hpp"

using emcool::can::CanSendCommand;
using emcool::can::ConsoleFrame;
using emcool::can::formatCanFrame;
using emcool::can::parseCanDumpFilter;
using emcool::can::parseCanSend;

namespace {

// A can-send/can-dump argument vector from words, as the console hands it over (argv[0] is the
// command name).
std::optional<std::string> runSend(std::vector<const char*> words, CanSendCommand& out) {
  return parseCanSend(static_cast<int>(words.size()), words.data(), out);
}

std::optional<std::string> runSend(std::vector<const char*> words, ConsoleFrame& out) {
  CanSendCommand command;
  const auto error = runSend(std::move(words), command);
  if (!error) {
    EMCOOL_CHECK(command.repeatMs == 0, "a send without -r should go once, not every %u ms",
                 static_cast<unsigned>(command.repeatMs));
    out = command.frame;
  }
  return error;
}

std::optional<std::string> runFilter(std::vector<const char*> words, std::optional<emcool::can::CanId>& out) {
  return parseCanDumpFilter(static_cast<int>(words.size()), words.data(), out);
}

void checkSend() {
  // A full eight-byte frame, id and bytes in mixed case, 0x prefixes allowed.
  {
    ConsoleFrame frame;
    const auto error = runSend({"can-send", "7df", "02", "01", "0C", "55", "55", "55", "55", "55"}, frame);
    EMCOOL_CHECK(!error, "a well-formed can-send was refused: %s", error ? error->c_str() : "");
    EMCOOL_CHECK(frame.id == 0x7df, "id %x", static_cast<unsigned>(frame.id));
    EMCOOL_CHECK(frame.length == 8, "length %u", frame.length);
    EMCOOL_CHECK(frame.data[0] == 0x02 && frame.data[1] == 0x01 && frame.data[2] == 0x0c, "bytes %u %u %u",
                 frame.data[0], frame.data[1], frame.data[2]);
  }
  {
    ConsoleFrame frame;
    const auto error = runSend({"can-send", "0x100", "0xAB"}, frame);
    EMCOOL_CHECK(!error, "a 0x-prefixed can-send was refused: %s", error ? error->c_str() : "");
    EMCOOL_CHECK(frame.id == 0x100 && frame.length == 1 && frame.data[0] == 0xab, "id %x len %u b0 %u",
                 static_cast<unsigned>(frame.id), frame.length, frame.data[0]);
  }
  // An id with no data bytes: a legal, empty frame.
  {
    ConsoleFrame frame;
    const auto error = runSend({"can-send", "123"}, frame);
    EMCOOL_CHECK(!error, "an empty-payload can-send was refused: %s", error ? error->c_str() : "");
    EMCOOL_CHECK(frame.id == 0x123 && frame.length == 0, "id %x len %u", static_cast<unsigned>(frame.id), frame.length);
  }
  // Refusals.
  {
    ConsoleFrame frame;
    EMCOOL_CHECK(runSend({"can-send"}, frame).has_value(), "can-send with no id was accepted");
    EMCOOL_CHECK(runSend({"can-send", "800"}, frame).has_value(), "an id past 7FF was accepted");
    EMCOOL_CHECK(runSend({"can-send", "7e8g"}, frame).has_value(), "a non-hex id was accepted");
    EMCOOL_CHECK(runSend({"can-send", "7df", "00", "00", "00", "00", "00", "00", "00", "00", "00"}, frame).has_value(),
                 "nine data bytes were accepted");
    EMCOOL_CHECK(runSend({"can-send", "7df", "1gg"}, frame).has_value(), "a non-hex byte was accepted");
    EMCOOL_CHECK(runSend({"can-send", "7df", "123"}, frame).has_value(), "a three-digit byte was accepted");
    // A one-digit byte is a friendly shorthand for its padded form (1 -> 0x01), deliberately allowed.
    EMCOOL_CHECK(!runSend({"can-send", "7df", "1"}, frame), "a one-digit byte should be read as 0x01");
  }
}

void checkExtended() {
  // Four to eight digits is an extended id, as cansend reads it: the PSU's diagnostic address.
  {
    ConsoleFrame frame;
    const auto error = runSend({"can-send", "18DAA0F1", "01", "3E"}, frame);
    EMCOOL_CHECK(!error, "an extended can-send was refused: %s", error ? error->c_str() : "");
    EMCOOL_CHECK(frame.extended && frame.id == 0x18daa0f1 && frame.length == 2, "ext %d id %x len %u", frame.extended,
                 static_cast<unsigned>(frame.id), frame.length);
  }
  {
    ConsoleFrame frame;
    EMCOOL_CHECK(!runSend({"can-send", "0x1FFFFFFF"}, frame) && frame.extended && frame.id == 0x1fffffff,
                 "the largest extended id was refused");
    // A short id padded to four digits is extended, as in cansend.
    EMCOOL_CHECK(!runSend({"can-send", "07DF"}, frame) && frame.extended && frame.id == 0x7df, "07DF should be extended");
    EMCOOL_CHECK(!runSend({"can-send", "7DF"}, frame) && !frame.extended, "7DF should be standard");
    EMCOOL_CHECK(runSend({"can-send", "20000000"}, frame).has_value(), "an id past 1FFFFFFF was accepted");
    EMCOOL_CHECK(runSend({"can-send", "118DAA0F1"}, frame).has_value(), "a nine-digit id was accepted");
  }
  {
    std::optional<emcool::can::CanId> id;
    EMCOOL_CHECK(!runFilter({"can-dump", "18DAF1A0"}, id) && id && id->extended && id->id == 0x18daf1a0,
                 "can-dump should filter on an extended id");
  }
  const std::array<std::uint8_t, 2> two = {0x01, 0x7e};
  EMCOOL_CHECK(formatCanFrame(0x18daf1a0, two, true) == "18DAF1A0 [2] 01 7E", "extended frame: %s",
               formatCanFrame(0x18daf1a0, two, true).c_str());
  EMCOOL_CHECK(formatCanFrame(0x7df, two, true) == "000007DF [2] 01 7E", "small extended id padded: %s",
               formatCanFrame(0x7df, two, true).c_str());
}

void checkRepeat() {
  // The PSU keep-on request, every 150 ms.
  {
    CanSendCommand command;
    const auto error = runSend({"can-send", "-r", "150", "500", "01", "00", "00", "00", "00", "00", "00", "00"}, command);
    EMCOOL_CHECK(!error, "a repeating can-send was refused: %s", error ? error->c_str() : "");
    EMCOOL_CHECK(command.repeatMs == 150, "repeat %u ms", static_cast<unsigned>(command.repeatMs));
    EMCOOL_CHECK(command.frame.id == 0x500 && command.frame.length == 8 && command.frame.data[0] == 0x01 &&
                     command.frame.data[7] == 0x00,
                 "id %x len %u b0 %u", static_cast<unsigned>(command.frame.id), command.frame.length,
                 command.frame.data[0]);
  }
  // The bounds are inclusive, and an empty frame can repeat too.
  {
    CanSendCommand command;
    EMCOOL_CHECK(!runSend({"can-send", "-r", "10", "123"}, command) && command.repeatMs == 10 &&
                     command.frame.length == 0,
                 "the 10 ms floor was refused");
    EMCOOL_CHECK(!runSend({"can-send", "-r", "60000", "123"}, command) && command.repeatMs == 60000,
                 "the 60 s ceiling was refused");
  }
  // Refusals.
  {
    CanSendCommand command;
    EMCOOL_CHECK(runSend({"can-send", "-r"}, command).has_value(), "-r with no interval was accepted");
    EMCOOL_CHECK(runSend({"can-send", "-r", "150"}, command).has_value(), "-r with no id was accepted");
    EMCOOL_CHECK(runSend({"can-send", "-r", "9", "500"}, command).has_value(), "a 9 ms repeat was accepted");
    EMCOOL_CHECK(runSend({"can-send", "-r", "60001", "500"}, command).has_value(), "a 60001 ms repeat was accepted");
    EMCOOL_CHECK(runSend({"can-send", "-r", "0", "500"}, command).has_value(), "a 0 ms repeat was accepted");
    EMCOOL_CHECK(runSend({"can-send", "-r", "0x96", "500"}, command).has_value(), "a hex repeat was accepted");
    EMCOOL_CHECK(runSend({"can-send", "-r", "15a", "500"}, command).has_value(), "a non-decimal repeat was accepted");
    EMCOOL_CHECK(runSend({"can-send", "-r", "999999999999", "500"}, command).has_value(),
                 "an overlong repeat was accepted");
    // -r goes before the id; after it, it is a (bad) data byte.
    EMCOOL_CHECK(runSend({"can-send", "500", "-r", "150"}, command).has_value(), "a trailing -r was accepted");
  }
}

void checkFilter() {
  {
    std::optional<emcool::can::CanId> id;
    const auto error = runFilter({"can-dump"}, id);
    EMCOOL_CHECK(!error && !id, "can-dump with no filter should leave the id unset");
  }
  {
    std::optional<emcool::can::CanId> id;
    const auto error = runFilter({"can-dump", "7E8"}, id);
    EMCOOL_CHECK(!error && id && id->id == 0x7e8 && !id->extended, "can-dump 7E8 should filter on standard 0x7e8");
  }
  {
    std::optional<emcool::can::CanId> id;
    EMCOOL_CHECK(runFilter({"can-dump", "800"}, id).has_value(), "an out-of-range dump filter was accepted");
    EMCOOL_CHECK(runFilter({"can-dump", "7e8", "extra"}, id).has_value(), "a second dump argument was accepted");
  }
}

void checkFormat() {
  const std::array<std::uint8_t, 3> three = {0x02, 0x01, 0x0c};
  EMCOOL_CHECK(formatCanFrame(0x7df, three) == "7DF [3] 02 01 0C", "three-byte frame: %s",
               formatCanFrame(0x7df, three).c_str());
  const std::array<std::uint8_t, 0> none = {};
  EMCOOL_CHECK(formatCanFrame(0x100, none) == "100 [0]", "empty frame: %s", formatCanFrame(0x100, none).c_str());
  const std::array<std::uint8_t, 2> small = {0x00, 0xff};
  EMCOOL_CHECK(formatCanFrame(0x7, small) == "007 [2] 00 FF", "small id padded: %s",
               formatCanFrame(0x7, small).c_str());
}

}  // namespace

int main() {
  checkSend();
  checkRepeat();
  checkExtended();
  checkFilter();
  checkFormat();
  if (emcool::test::failures() == 0) std::printf("check_can_console: ok\n");
  return emcool::test::failures() == 0 ? 0 : 1;
}
