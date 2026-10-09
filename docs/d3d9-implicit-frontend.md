# Implicit owner device frontend

With `PW_D3D9_ENABLE_IMPLICIT`, successful Create obtains the service's actual
LIST and installs implicit owner identities before exposing the device. Failure
cancels/joins the session and removes frontend owner metadata before releasing
registration ownership. The paired builder enables the feature in both DLLs
and records feature mask 6143. Feature-disabled code retains previous behavior.

Reset holds its existing self-reference and transition gate. It freezes
public-zero owned shells locally, sends PREPARE, then invokes actual Reset and
FINISH. RESTORED thaws existing identities; RETIRED retires the old identities
and installs the service's new owner list. A failed Reset can return RETIRED
when the backend already destroyed old owners. The backend Reset HRESULT stays
separate from the ownership-control status. A failed backend HRESULT is retained
even if final control cleanup also fails; successful backend Reset becomes a
failure if ownership cannot be reconciled. Missing/malformed replies cancel and
stick the device and only thaw frontend metadata, without claiming native
restoration. PREPARE failure thaws before returning. Reentrant Reset is rejected
before freezing or sending any ownership control operation.

Final Release closes owner admission and retains the helper's private teardown
sentinel. Callback reentry queues final cleanup **before any disposal RPC**.
Outside callback admission, cleanup disposes frontend owner shells, sends DRAIN,
then releases the native device, unregisters the guest window, and frees the
shell directly. It never Releases the private sentinel. Failed defer retains the
shell/parent and cancels, allowing a safe retry instead of freeing live ownership.

`tests/lab/d3d9_implicit_frontend.py` runs the actual frontend in PE32 and PE64
with controlled service/owner replies and real USER windows. It covers early
PREPARE rejection, RESTORED/RETIRED backend failures, replacement installation,
missing control/Reset replies, callback rejection, sentinel preservation,
deferred cleanup ordering and failed enqueue retention. Actual backend and
production transport acceptance is a separate fixture.
