4.8 Reset domain management protocol
This protocol is intended for control of reset capable domains in the platform. The reset management
protocol provides commands to:
• Describe the protocol version.
• Discover the attributes and capabilities of the reset domains in the system.
• Reset a given domain.
• Receive notifications when a given domain is reset.
4.8.1 Reset domain management protocol background
Devices that can be collectively reset through a common reset signal constitute a reset domain. A reset
domain can be reset autonomously or explicitly. When autonomous reset is chosen, the firmware is
responsible for taking the necessary steps to reset the domain and to subsequently bring it out of reset.
When explicit reset is chosen, the caller has to specifically assert and then de-assert the reset signal by
issuing two separate RESET commands.
Reset State encoding for reset domains is described below in Table 18.
Table 18: Reset State Parameter Layout
Bit field
Description
Reset Type
31
If set to 0, indicates Architectural Reset.
If set to 1, indicates IMPLEMENTATION defined Reset.
30:0
Reset ID
The two distinct reset types possible are architectural reset and IMPLEMENTATION defined reset.
Reset Types and Reset IDs are described in Table 19.
Table 19: Reset Type and Reset ID Description
Reset Type
Reset ID Description
0x0 COLD_RESET.
Full loss of context of all devices in the
domain.
Architectural Reset
0x1-0x7FFFFFFF
Reserved for future use.
Lower values indicate greater context loss.
IMPLEMENTATION defined Resets.
IMPLEMENTATION
defined Reset
0x0-0x7FFFFFFF
All values represent resets that result in
varying levels of context loss.
Lower values indicate greater context loss.
Page 112 of 136
Copyright © 2017 - 2020 Arm Limited or its affiliates. All rights reserved.
Non-Confidential
DEN0056CSystem Control and Management Interface
Reset domains are not the same as power domains, although they can be the same. There could be
multiple reset domains within a given power domain. There could also be reset domains that straddle
multiple power domains.
Resets might impose the requirement that devices in the affected reset domain are in a state of
quiescence before the reset is issued. Support for such quiescence might be provided by the reset
domain. In the absence of such a support, it is the calling agent’s responsibility to ensure quiescence
prior to invocation of the reset.
Protocol commands take integer identifiers to identify the reset domain they apply to. The identifiers are
sequential and start from 0.
4.8.2 Commands
4.8.2.1 PROTOCOL_VERSION
On success, this command returns the version of this protocol. For this version of the specification, the
value returned must be 0x20000, which corresponds to version 2.0.
message_id: 0x0
protocol_id: 0x16
This command is mandatory.
Return values
Name Description
int32 status See section 4.1.4 for status code definitions.
uint32 version For this revision of the specification, this must be 0x20000 .
4.8.2.2 PROTOCOL_ATTRIBUTES
This command returns the implementation details associated with this protocol.
message_id: 0x1
protocol_id: 0x16
This command is mandatory.
Return values
Name Description
int32 status See section 4.1.4 for status code definitions.
uint32 attributes
Page 113 of 136
Bits[31:16] Reserved, must be zero.
Bits[15:0]
Number of reset domains.
Copyright © 2017 - 2020 Arm Limited or its affiliates. All rights reserved.
Non-Confidential
DEN0056CSystem Control and Management Interface
4.8.2.3 PROTOCOL_MESSAGE_ATTRIBUTES
On success, this command returns the implementation details associated with a specific message in
this protocol.
message_id: 0x2
protocol_id: 0x16
This command is mandatory.
Parameters
Name Description
uint32 message_id message_id of the message.
Return values
Name
Description
One of, but not limited to, the following:
int32 status
• SUCCESS: in case the message is implemented and
available to use.
• NOT_FOUND: if the message identified by message_id
is not provided by this platform implementation.
See section 4.1.4 for more status code definitions.
uint32 attributes
Reserved, must be zero.
4.8.2.4 RESET_DOMAIN_ATTRIBUTES
This command returns attributes of the reset domain specified in the command.
message_id: 0x3
protocol_id: 0x16
This command is mandatory.
Parameters
Name Description
uint32 domain_id Identifier for the reset domain.
Return values
Name
Description
int32 status
Page 114 of 136
One of, but not limited to, the following:
•
SUCCESS: if valid reset domain attributes were returned.
Copyright © 2017 - 2020 Arm Limited or its affiliates. All rights reserved.
Non-Confidential
DEN0056CSystem Control and Management Interface
•
NOT_FOUND: if domain_id pertains to a non-existent
domain.
See section 4.1.4 for more status code definitions.
Bit[31]
Asynchronous reset support.
Set to 1 if this domain can be reset
asynchronously.
Set to 0 if this domain can only be reset
synchronously.
uint32 attributes
Bit[30]
Reset notifications support.
Set to 1 if reset notifications are supported for
this domain.
Set to 0 if reset notifications are not supported
for this domain.
Bits[29:0]
Reserved, must be zero.
uint32 latency Maximum time (in microseconds) required for the reset to
take effect on the given domain. A value of 0xFFFFFFFF
indicates this field is not supported by the platform.
uint8 name[16] Null-terminated ASCII string of up to 16 bytes in length
describing the reset domain name.
4.8.2.5 RESET
This command allows an agent to reset the specified reset domain. If the reset request is issued as an
asynchronous call, the platform must return immediately upon receipt of the request. The platform might
need to ensure that the domain and all dependent logic have reached a state of quiescence before
performing the actual reset, although this is not mandatory.
When the reset is done, the platform should then send a RESET_COMPLETE delayed response,
described in section 4.8.3.1.The platform has the option to inform agents other than the caller of the
reset incident, using the RESET_ISSUED notification that is described in section 4.8.4.1.
message_id: 0x4
protocol_id: 0x16
This command is mandatory.
Parameters
Name Description
uint32 domain_id Identifier for the reset domain.
Page 115 of 136
Copyright © 2017 - 2020 Arm Limited or its affiliates. All rights reserved.
Non-Confidential
DEN0056CSystem Control and Management Interface
This parameter allows the agent to specify additional
conditions and requirements specific to the request, and has
the following format:
Bits[31:3] Reserved, must be zero.
Bit[2] Async flag. Only valid if Bit[0] is set to 1.
Set to 1 if the reset must complete
asynchronously.
Set to 0 if the reset must complete
synchronously.
uint32 flags
Bit[1]
Explicit signal. This flag is ignored when
Bit[0] is set to 1.
Set to 1 to explicitly assert reset signal.
Set to 0 to explicitly de-assert reset signal.
Bit[0]
Autonomous Reset action.
Set to 1 if the reset must be performed
autonomously by the platform.
Set to 0 if the reset signal shall be explicitly
asserted and de-asserted by the caller.
uint32 reset_state
The reset state being requested. The format of this
parameter is specified in Table 18.
Return values
Name
Description
One of, but not limited to, the following:
• SUCCESS: if the operation was successful.
• NOT_FOUND: if the reset domain identified by
domain_id does not exist.
• INVALID_PARAMETERS: if an illegal or unsupported
reset state is specified or if the flags field is invalid.
• GENERIC_ERROR: if the operation failed, for
example if there are other active users of the reset
domain.
• DENIED: if the calling agent is not allowed to reset
the specified reset domain.
int32 status
See section 4.1.4 for more status code definitions.
4.8.2.6 RESET_NOTIFY
This command allows the caller to request notifications from the platform when a reset domain has
been reset. If reset has been explicitly signaled, the platform generates this notification when the reset
Page 116 of 136
Copyright © 2017 - 2020 Arm Limited or its affiliates. All rights reserved.
Non-Confidential
DEN0056CSystem Control and Management Interface
signal has been asserted. These notifications are sent using the RESET_ISSUED notification, which is
described in section 4.8.4.1.
Notification support is optional, and PROTOCOL_MESSAGE_ATTRIBUTES must be used to discover
whether this command is implemented.
These notifications must be disabled by default during initial boot of the platform.
message_id: 0x5
protocol_id: 0x16
This command is optional.
Parameters
Name Description
uint32 domain_id Identifier for the reset domain.
Bits[31:1] Reserved must be zero.
Bit[0] Notify enable. This bit can have one of the
following values:
1, which indicates that the platform should send
RESET_ISSUED notifications to the calling
agent when the domain is reset.
uint32 notify_enable
0, which indicates that the platform should not
send any RESET_ISSUED notifications to the
calling agent.
Return values
Name
Description
One of, but not limited to, the following:
• SUCCESS.
• NOT_FOUND: if domain_id does not point to a valid
domain.
• INVALID_PARAMETERS: if notify_enable specifies
values that are either illegal or incorrect.
int32 status
See section 4.1.4 for more status code definitions.
4.8.3 Delayed Responses
4.8.3.1 RESET_COMPLETE
The platform sends this delayed response to the caller that requested an asynchronous reset of the
specified domain.
Page 117 of 136
Copyright © 2017 - 2020 Arm Limited or its affiliates. All rights reserved.
Non-Confidential
DEN0056CSystem Control and Management Interface
message_id: 0x4
protocol_id: 0x16
This command is optional.
Parameters
Name
Description
One of, but not limited to, the following:
• SUCCESS: if reset was successful.
• GENERIC_ERROR: if the operation failed, for example if
there were other users of the reset domain, or if the
domain could not be brought to a state of quiescence
preparatory to the reset.
int32 status
Other vendor-specific errors can also be generated
depending on the implementation.
See section 4.1.4 for more status code definitions.
uint32 domain_id
Identifier for the reset domain.
4.8.4 Notifications
4.8.4.1 RESET_ISSUED
The platform sends this notification to an agent that has registered to receive notifications when the
reset domain identified by domain_id has been reset. The notification might not be received if the agent
is affected as a result of the reset.
message_id: 0x0
protocol_id: 0x16
This command is optional.
Parameters
Name Description
uint32 agent_id Identifier of the agent that caused the reset of the domain.
uint32 domain_id Identifier of the reset domain.
uint32 reset_state The reset state issued on the domain. The format of this
parameter is specified in Table 18.