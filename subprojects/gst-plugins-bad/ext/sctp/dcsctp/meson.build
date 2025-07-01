# third party libraries
subdir('third_party')

# dcSCTP
dcsctp_sources = [
  # absl
  'absl/base/internal/raw_logging.cc',
  'absl/strings/ascii.cc',
  'absl/strings/charconv.cc',
  'absl/strings/internal/charconv_bigint.cc',
  'absl/strings/internal/charconv_parse.cc',
  'absl/strings/internal/memutil.cc',
  'absl/strings/match.cc',
  'absl/strings/numbers.cc',

  # rtc_base
  'rtc_base/memory/aligned_malloc.cc',
  'rtc_base/logging.cc',
  'rtc_base/checks.cc',
  'rtc_base/platform_thread_types.cc',
  'rtc_base/time_utils.cc',
  'rtc_base/system_time.cc',
  'rtc_base/string_encode.cc',
  'rtc_base/string_utils.cc',
  'rtc_base/strings/string_builder.cc',
  'rtc_base/strings/string_format.cc',
  'rtc_base/synchronization/sequence_checker_internal.cc',

  # api
  'api/task_queue/task_queue_base.cc',
  'api/task_queue/pending_task_safety_flag.cc',
  'api/units/time_delta.cc',
  'api/units/timestamp.cc',

  # public
  'net/dcsctp/public/dcsctp_handover_state.cc',
  'net/dcsctp/public/dcsctp_socket_factory.cc',
  'net/dcsctp/public/text_pcap_packet_observer.cc',

  # packet
  'net/dcsctp/packet/parameter/add_incoming_streams_request_parameter.cc',
  'net/dcsctp/packet/parameter/add_outgoing_streams_request_parameter.cc',
  'net/dcsctp/packet/parameter/forward_tsn_supported_parameter.cc',
  'net/dcsctp/packet/parameter/heartbeat_info_parameter.cc',
  'net/dcsctp/packet/parameter/incoming_ssn_reset_request_parameter.cc',
  'net/dcsctp/packet/parameter/outgoing_ssn_reset_request_parameter.cc',
  'net/dcsctp/packet/parameter/parameter.cc',
  'net/dcsctp/packet/parameter/reconfiguration_response_parameter.cc',
  'net/dcsctp/packet/parameter/ssn_tsn_reset_request_parameter.cc',
  'net/dcsctp/packet/parameter/state_cookie_parameter.cc',
  'net/dcsctp/packet/parameter/supported_extensions_parameter.cc',
  'net/dcsctp/packet/parameter/zero_checksum_acceptable_chunk_parameter.cc',
  'net/dcsctp/packet/error_cause/cookie_received_while_shutting_down_cause.cc',
  'net/dcsctp/packet/error_cause/error_cause.cc',
  'net/dcsctp/packet/error_cause/invalid_mandatory_parameter_cause.cc',
  'net/dcsctp/packet/error_cause/invalid_stream_identifier_cause.cc',
  'net/dcsctp/packet/error_cause/missing_mandatory_parameter_cause.cc',
  'net/dcsctp/packet/error_cause/no_user_data_cause.cc',
  'net/dcsctp/packet/error_cause/out_of_resource_error_cause.cc',
  'net/dcsctp/packet/error_cause/protocol_violation_cause.cc',
  'net/dcsctp/packet/error_cause/restart_of_an_association_with_new_address_cause.cc',
  'net/dcsctp/packet/error_cause/stale_cookie_error_cause.cc',
  'net/dcsctp/packet/error_cause/unrecognized_chunk_type_cause.cc',
  'net/dcsctp/packet/error_cause/unrecognized_parameter_cause.cc',
  'net/dcsctp/packet/error_cause/unresolvable_address_cause.cc',
  'net/dcsctp/packet/error_cause/user_initiated_abort_cause.cc',
  'net/dcsctp/packet/chunk/abort_chunk.cc',
  'net/dcsctp/packet/chunk/chunk.cc',
  'net/dcsctp/packet/chunk/cookie_ack_chunk.cc',
  'net/dcsctp/packet/chunk/cookie_echo_chunk.cc',
  'net/dcsctp/packet/chunk/data_chunk.cc',
  'net/dcsctp/packet/chunk/error_chunk.cc',
  'net/dcsctp/packet/chunk/forward_tsn_chunk.cc',
  'net/dcsctp/packet/chunk/heartbeat_ack_chunk.cc',
  'net/dcsctp/packet/chunk/heartbeat_request_chunk.cc',
  'net/dcsctp/packet/chunk/idata_chunk.cc',
  'net/dcsctp/packet/chunk/iforward_tsn_chunk.cc',
  'net/dcsctp/packet/chunk/init_ack_chunk.cc',
  'net/dcsctp/packet/chunk/init_chunk.cc',
  'net/dcsctp/packet/chunk/reconfig_chunk.cc',
  'net/dcsctp/packet/chunk/sack_chunk.cc',
  'net/dcsctp/packet/chunk/shutdown_ack_chunk.cc',
  'net/dcsctp/packet/chunk/shutdown_chunk.cc',
  'net/dcsctp/packet/chunk/shutdown_complete_chunk.cc',
  'net/dcsctp/packet/tlv_trait.cc',
  'net/dcsctp/packet/crc32c.cc',
  'net/dcsctp/packet/chunk_validators.cc',
  'net/dcsctp/packet/sctp_packet.cc',

  # rx
  'net/dcsctp/rx/data_tracker.cc',
  'net/dcsctp/rx/interleaved_reassembly_streams.cc',
  'net/dcsctp/rx/traditional_reassembly_streams.cc',
  'net/dcsctp/rx/reassembly_queue.cc',

  # socket
  'net/dcsctp/socket/heartbeat_handler.cc',
  'net/dcsctp/socket/stream_reset_handler.cc',
  'net/dcsctp/socket/packet_sender.cc',
  'net/dcsctp/socket/transmission_control_block.cc',
  'net/dcsctp/socket/callback_deferrer.cc',
  'net/dcsctp/socket/dcsctp_socket.cc',
  'net/dcsctp/socket/state_cookie.cc',

  # timer
  'net/dcsctp/timer/timer.cc',
  'net/dcsctp/timer/task_queue_timeout.cc',

  # tx
  'net/dcsctp/tx/rr_send_queue.cc',
  'net/dcsctp/tx/stream_scheduler.cc',
  'net/dcsctp/tx/retransmission_error_counter.cc',
  'net/dcsctp/tx/retransmission_timeout.cc',
  'net/dcsctp/tx/outstanding_data.cc',
  'net/dcsctp/tx/retransmission_queue.cc',

  # C wrapper
  'sctpsocket.cc',
]

threads_dep = dependency('threads')

dcsctp_cppargs = [
  '-DWEBRTC_ALLOW_DEPRECATED_NAMESPACES',
]
dcsctp_deps = [threads_dep, crc32c_dep]

if host_system == 'windows'
  dcsctp_cppargs += [
    # fixes winsock redeclaration issue
    '-DWIN32_LEAN_AND_MEAN',
    '-DWEBRTC_WIN',
    '-DNOMINMAX',
    '/std:c++20',
  ]
  dcsctp_sources += [
    'rtc_base/win32.cc',
  ]
  dcsctp_deps += [
    cc.find_library('winmm'),
  ]
else
  dcsctp_cppargs += [
    '-DWEBRTC_POSIX',
    '-std=c++17',
     '-Wall',
    '-Werror',
    '-Wno-comment',
  ]

  if host_system == 'linux'
    dcsctp_cppargs += [
      '-DWEBRTC_LINUX',
    ]
  elif host_system == 'android'
    dcsctp_cppargs += [
      '-DWEBRTC_ANDROID',
      '-DWEBRTC_LINUX',
    ]
  elif host_system == 'darwin'
    dcsctp_cppargs += [
      '-DWEBRTC_MAC',
    ]
  elif host_system == 'ios'
    dcsctp_cppargs += [
      '-DWEBRTC_IOS',
    ]
  endif
endif

# work around for windows longpaths issue
sources = []
foreach source :  dcsctp_sources
  sources += [join_paths(meson.current_source_dir(), source)]
endforeach
dcsctp_sources = sources

dcsctp_lib = static_library('dcsctp',
  dcsctp_sources,
  cpp_args: dcsctp_cppargs,
  dependencies: dcsctp_deps,
  install: false,
  include_directories: include_directories('.'))

dcsctp_dep = declare_dependency(link_with: dcsctp_lib,
  include_directories : include_directories('.'))
