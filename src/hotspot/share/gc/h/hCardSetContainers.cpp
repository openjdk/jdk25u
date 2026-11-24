#include "gc/h/hCardSetContainers.hpp"
#include "utilities/globalDefinitions.hpp"

uint HGCCardSetContainer::LogCardsPerRegionLimit = sizeof(HGCCardSetArray::EntryDataType) * BitsPerByte;
