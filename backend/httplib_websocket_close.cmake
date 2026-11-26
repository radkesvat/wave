# cpp-httplib 0.67.0 sends Close from another thread but leaves an in-progress
# read waiting for the rest of its frame. Keep the fix in the build directory.
if(HTTPLIB_COMPILE)
    set(httplib_close_input "${httplib_BINARY_DIR}/out/httplib.cc")
    set(httplib_close_output "${CMAKE_CURRENT_BINARY_DIR}/httplib.cc")
    set_property(SOURCE "${httplib_close_input}" TARGET_DIRECTORY httplib PROPERTY HEADER_FILE_ONLY TRUE)
    target_sources(httplib PRIVATE "${httplib_close_output}")
else()
    set(httplib_close_input "${httplib_SOURCE_DIR}/httplib.h")
    set(httplib_close_output "${CMAKE_CURRENT_BINARY_DIR}/httplib/httplib.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${httplib_close_input}")
    target_include_directories(httplib SYSTEM BEFORE INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/httplib>")
endif()

function(wave_replace_httplib original replacement)
    string(FIND "${httplib_close_source}" "${original}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Check the WebSocket close fix against this version of cpp-httplib.")
    endif()
    string(REPLACE "${original}" "${replacement}" result "${httplib_close_source}")
    set(httplib_close_source "${result}" PARENT_SCOPE)
endfunction()

file(READ "${httplib_close_input}" httplib_close_source)
wave_replace_httplib("  if (!read_lock.owns_lock()) { return; }" [=[  if (!read_lock.owns_lock()) {
    // Reuse the heartbeat's read interrupt to wake the active frame reader after sending Close.
    pong_timed_out_ = true;
    return;
  }]=])
wave_replace_httplib("      auto ret = tls::read(session_, ptr, size, err);" [=[      // Already-buffered TLS records can be read without polling the socket.
      if (read_interrupted_ && read_interrupted_()) {
        error_ = Error::Canceled;
        return -1;
      }
      auto ret = tls::read(session_, ptr, size, err);]=])
file(CONFIGURE OUTPUT "${httplib_close_output}" CONTENT "@httplib_close_source@" @ONLY)
