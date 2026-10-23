#include "snapshot.h"

#include "display.h"

const RangeData& snapshotRange(const Snapshot& s, uint8_t range) {
  switch (range) {
    case RS_TODAY: return s.today;
    case RS_YESTERDAY: return s.y;
    case RS_D7: return s.d7;
    case RS_D30: return s.d30;
    default: return s.today;
  }
}
