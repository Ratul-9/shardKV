// lincheck: checks a history file written by `kvctl history` for linearizability (Phase 10).
//   lincheck history.txt     exit code 0 = linearizable, 1 = violation, 3 = gave up, 2 = bad input
#include <cstdio>

#include "lldstore/linearizability.h"
#include "lldstore/log_file.h"

int main(int argc, char** argv) {
  using namespace lldstore;
  if (argc != 2) {
    std::fprintf(stderr, "usage: lincheck HISTORY_FILE\n");
    return 2;
  }
  std::string text, error;
  if (!ReadWholeFile(argv[1], &text)) {
    std::fprintf(stderr, "cannot read %s\n", argv[1]);
    return 2;
  }
  std::vector<lin::Operation> history;
  if (!lin::ParseHistory(text, &history, &error)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 2;
  }
  size_t unknown = 0;
  for (const auto& op : history) unknown += op.known ? 0 : 1;
  const lin::CheckResult r = lin::Check(history);
  std::printf("%s: %zu operations (%zu with unknown outcome), %llu search steps: %s", argv[1],
              history.size(), unknown, static_cast<unsigned long long>(r.steps), lin::VerdictName(r.verdict));
  if (r.verdict != lin::Verdict::kLinearizable) std::printf(" (key \"%s\")", r.key.c_str());
  std::printf("\n");
  return r.verdict == lin::Verdict::kLinearizable ? 0 : r.verdict == lin::Verdict::kViolation ? 1 : 3;
}
