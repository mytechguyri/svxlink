###############################################################################
#
# TetraLogic event handlers
#
###############################################################################

#
# This is the namespace in which all functions below will exist. The name
# must match the corresponding section "[TetraLogic]" in the configuration
# file. The name may be changed but it must be changed in both places.
#
namespace eval TetraLogic {

#
# Executed when a DTMF command has been received
#   cmd - The command
#
# Return 1 to hide the command from further processing is SvxLink or
# return 0 to make SvxLink continue processing as normal.
# ---> must return 0!
# adapt the port according to your needs
#
proc dtmf_cmd_received {cmd} {
  variable port;
  variable number;

  if {[string length $cmd] > 5 && [string range $cmd 0 1] != 91} {
    puts "### dialing number $cmd via sip pty";
    # adjust the variable port according to your specifications
    # "/tmp/ctrl" must be match to SIP_CTRL_PTY in [SipLogic]-section
    set port [open "/tmp/sipctrl" w+];
      set number "C$cmd";
      puts $port $number;
    close $port;
  }

  # if a call isn't aswered you can stop the call by sending an other ssd, eg
  # [SdsToCommand]
  # 32771=12
  # the next section will end the pending call
  if {$cmd == "12"} {
    set port [open "/tmp/sipctrl" w+];
      set number "C#";
      puts $port $number;
    close $port;
  }
  return 0;
}

# end of namespace
}

#
# This file has not been truncated
#
