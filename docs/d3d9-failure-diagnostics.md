# D3D9 failure diagnostics

The service logs failed factory calls with the native method name, HRESULT and
all copied scalar inputs. Successful calls, including S_FALSE, add no logs.
Texture native adapters log completed dispatch failures with the operation and
resource parameters using the same helper. Early validation and registry failures
retain the existing transaction diagnostic.
No pointers, shader contents, texture bytes or private application paths appear.

These records attribute capability-probe failures without changing HRESULTs or
classifying them as harmless. The host test checks failure names, parameters,
unsigned values and silence on success. Console acceptance still requires the
actual production service result; a passing smoke alone does not explain every
negative backend HRESULT.
