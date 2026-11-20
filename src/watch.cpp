#include <iterator>
#include <vector>

#include "screenlight.h"

namespace screenlight {
namespace {

void append(std::vector<Watcher>* all, std::vector<Watcher> more) {
  all->insert(all->end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
}

}  // namespace

std::vector<Watcher> collect_watchers() {
  const Settings& s = settings();
  std::vector<Watcher> all;
  if (s.watch_screen) append(&all, duplicating_processes());
  if (s.watch_remote) {
    append(&all, hidden_desktop_processes());
    append(&all, remote_viewers());
  }
  if (s.watch_input) append(&all, input_senders());
  for (Watcher& w : service_watchers()) {
    if (watched(w)) all.push_back(std::move(w));
  }
  return all;
}

}  // namespace screenlight
