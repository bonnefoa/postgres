#include "postgres.h"

#include "tcop/dest.h"

extern bool printcompressed(TupleTableSlot *slot, DestReceiver *self);
extern void printcompressed_startup(DestReceiver *self, int operation,
								TupleDesc tupdesc);
extern void printcompressed_cleanup(DestReceiver *self);
extern void printcompressed_shutdown(DestReceiver *self);
