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
  std::vector<Watcher> all = duplicating_processes();
  append(&all, hidden_desktop_processes());
  append(&all, remote_viewers());
  return all;
}

}  // namespace screenlight
