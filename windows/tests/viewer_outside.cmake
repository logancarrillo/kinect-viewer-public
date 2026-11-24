# W8 (pet-camera.md §4, the outside rows; protocol v3 §8, §10): the Pet camera panel's "Outside the home" section and
# its windows, synthetic only. The one launch passes --synthetic and a --state-dir under the build tree, so
# pet-camera.json, the invite and the connection log stay there; it binds every listener on loopback
# (--test-outside implies --loopback-only), and the host gets the viewer's fake router through
# Options::router_factory, so nothing here reaches a real router, not even to read.
#
# The hook's scripted pass hands the panel a view for every status and checks it control by control: off disables
# everything, sharing off and an unreadable pet-camera.json disable the switch with the reason, each status's
# sentence (the carrier-NAT text included), "Checking...", the manual steps and the "Public address" field labelled
# "entered by you, not checked", the address-changed line and Re-share, and Invite gated on canInvite.
# The windows on made-up links (no real token), in every build: the invite window masked, with its countdown and
# warning, the screenshot hook refusing while it is open, and the window closing itself because the host has no such
# invite; the re-share window acknowledging on Done; the connection log window.
# Then, when this build's host has the outside interface (ot_host_interface=1), the same panel against the host and
# the fake router: mapped; an invite shown (masked: a test run never shows a live token), the screenshot hook
# refusing while it is open, Cancel, and a second invite the host closes, which the window notices; the connection
# log window; the address-changed flow and Re-share's endpoint link (no secret) acknowledged; off disabling
# everything and removing the mapping; carrier NAT; manual, a private public address refused and a public one taken
# as "entered by you, not checked"; and a port another machine holds. "Remove mapping now" stays removed: the
# forced 10-minute check adds nothing, and "Map the port again" maps it.
#
# The report never holds the invite's link: only that it was shown, masked, and its length. Checked below.
# The viewer's exit code must be 0, so a crash or a hang at exit fails the test.
cmake_minimum_required(VERSION 3.20)

function(expect)
    foreach(want IN LISTS ARGN)
        if(NOT report MATCHES "${want}")
            message(FATAL_ERROR "expected '${want}' in the report")
        endif()
    endforeach()
endfunction()

file(REMOVE_RECURSE "${STATE_DIR}")
file(MAKE_DIRECTORY "${STATE_DIR}")
file(WRITE "${STATE_DIR}/.kinect-viewer-host-state" "kinect-viewer host state\n")
file(REMOVE "${OUT}" "${OUT}.txt" "${OUT}.refused.txt")

execute_process(
    COMMAND "${VIEWER}" --synthetic --mode depth --test-outside --state-dir "${STATE_DIR}" --screenshot "${OUT}"
            --exit-after-screenshot
    TIMEOUT 240
    RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "the viewer exited with ${rc}")
endif()
if(NOT EXISTS "${OUT}.txt")
    message(FATAL_ERROR "no report written")
endif()
file(READ "${OUT}.txt" report)
message(STATUS "${report}")
# The capture itself must exist, and ok=1 must be a whole line of its own (not, say, the end of "shot_ok=1").
if(NOT EXISTS "${OUT}")
    message(FATAL_ERROR "the report says the panel was captured, but ${OUT} isn't there")
endif()
if(NOT report MATCHES "(^|\n)ok=1\n")
    message(FATAL_ERROR "the report has no line ok=1")
endif()
# v3 §10: the host of a test run uses the fixed test LAN address, never this PC's own adapters.
if(NOT report MATCHES "ot_lan_test_address=1\n")
    message(FATAL_ERROR "the host didn't use the test LAN address 192.168.77.10")
endif()

# v3 §10: no invite link, token or masked, ever reaches the report.
if(report MATCHES "tok=" OR report MATCHES "kv2://invite")
    message(FATAL_ERROR "the report holds an invite link")
endif()
if(NOT report MATCHES "state_dir=[^\n]*viewer-outside\n")
    message(FATAL_ERROR "the run didn't keep its state in ${STATE_DIR}")
endif()

expect("ot_ran=1" "ot_done=1" "ot_failed=\n" "synthetic=1"
       "ot_s_off=1" "ot_s_sharing_off=1" "ot_s_save_error=1" "ot_s_mapped=1" "ot_s_manual_field=1" "ot_s_manual_entered=1"
       "ot_s_error=1" "ot_s_carrier=1" "ot_s_address_changed=1" "ot_s_checking=1" "ot_off_at_start=1" "ot_sharing_off_reason=1"
       "ot_w_invite_shown=1" "ot_w_invite_masked=1" "ot_w_invite_countdown=1" "ot_w_invite_warning=1" "ot_w_invite_role=1"
       "ot_w_shot_refused=1" "ot_w_self_closed=1" "ot_w_shot_allowed_after=1" "ot_w_reshare_shown=1"
       "ot_w_reshare_acknowledged=1" "ot_w_log_opened=1"
       "ot_s_remove_enabled=1" "ot_s_remove_disabled_off=1" "ot_s_profile_hint=1" "ot_s_profile_hint_off=1"
       "ot_s_remap=1" "ot_s_no_port_label=1")

# The host has the outside interface in every build now: its half of the hook must run.
if(NOT report MATCHES "ot_host_interface=1")
    message(FATAL_ERROR "the host has no outside interface: its half of the hook didn't run")
endif()
if(report MATCHES "ot_host_interface=1")
    expect("ot_router_injected=1" "ot_mapped=1" "ot_mapped_endpoint=1" "ot_mapped_invite_enabled=1"
           "ot_invite_shown=1" "ot_invite_link_ok=1" "ot_invite_masked=1" "ot_invite_token_hidden=1" "ot_invite_countdown=1"
           "ot_invite_warning=1" "ot_invite_role_viewer=1" "ot_invite_still_open=1" "ot_shot_refused_invite=1"
           "ot_invite_cancelled=1" "ot_invite_window_gone=1" "ot_shot_allowed_after_invite=1" "ot_invite_role_owner=1"
           "ot_invite_self_closed=1" "ot_log_opened=1" "ot_invite_capture_excluded=1"
           "ot_changed_shown=1" "ot_reshare_opened=1" "ot_reshare_link_ok=1" "ot_reshare_no_secret=1" "ot_changed_acknowledged=1"
           "ot_off_disables=1" "ot_mapping_removed=1" "ot_carrier_nat=1" "ot_carrier_invite_disabled=1"
           "ot_manual=1" "ot_manual_steps=1" "ot_manual_field=1" "ot_manual_bad_refused=1" "ot_manual_entered=1"
           "ot_manual_unverified=1" "ot_manual_invite_enabled=1" "ot_error_taken=1"
           "ot_invite_closed_reason=1" "ot_remove_enabled=1" "ot_remove_mapping=1" "ot_remove_listening=1"
           "ot_remap_enabled_removed=1" "ot_recheck_kept_removed=1" "ot_remapped=1")
    # The host used the fake router and nothing else: it opened sessions on it.
    if(NOT report MATCHES "ot_router_opens=[1-9]")
        message(FATAL_ERROR "the host never opened the fake router")
    endif()
    if(NOT report MATCHES "ot_invite_link_length=[1-9][0-9][0-9]?\n")
        message(FATAL_ERROR "the invite's length wasn't reported")
    endif()
endif()

file(REMOVE_RECURSE "${STATE_DIR}")
