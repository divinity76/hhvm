HHVM_DEFINE_EXTENSION("thrift"
  SOURCES
    adapter.cpp
    field_adapter.cpp
    binary.cpp
    compact.cpp
    ext_thrift.cpp
    spec-holder.cpp
  HEADERS
    adapter.h
    field_adapter.h
    util.h
    ext_thrift.h
    spec-holder.h
    transport.h
  DEPENDS
    libThrift
  SYSTEMLIB
    ext_thrift.php
)
