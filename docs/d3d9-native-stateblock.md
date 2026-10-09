# Native stateblock execution

The helper calls actual native CreateStateBlock, BeginStateBlock, EndStateBlock,
Capture and Apply on the validated owner-thread target. Create/End return one
owned native interface to the service registry. The service supplies parent-device
lifetime and canonical identity ownership. Other methods leave the creation output
untouched. Exact backend HRESULTs survive. Failed creation with an unexpectedly
retained output is released; successful creation without an object returns E_FAIL.

Focused controlled native-vtable and ASan/UBSan fixtures pass all five calls,
positive/failing HRESULTs, invalid requests, no-object success and failure cleanup.
Three actual PE32 bootstrap / PE64 DXVK processes each restore state three times:
Create ALL / Apply, Capture / Apply, and Begin/End selective recording / Capture /
Apply. Native GetDevice returns the actual parent device identity.

Receipt: `/tmp/prospero-d3d9-native-stateblock-r1/receipt.json`.
This proof covers native backend execution. Production stateblock proxy/session
integration and console gameplay remain separate checks.
