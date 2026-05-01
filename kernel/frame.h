#ifndef FRAME_H
#define FRAME_H

#include "types.h"

struct proc; // forward declaration

struct frame_entry {
  int in_use;
  struct proc *owner;
  uint64 va;
  int ref_bit;
};

#endif