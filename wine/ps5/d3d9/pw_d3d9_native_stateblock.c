/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define COBJMACROS
#include "pw_d3d9_native_stateblock.h"
HRESULT pw_d3d9_native_stateblock_dispatch(IDirect3DDevice9 *device,IDirect3DStateBlock9 *block,
 const struct pw_d3d9_stateblock_request *q,IDirect3DStateBlock9 **created)
{
 IDirect3DStateBlock9 *result=NULL;HRESULT hr;int status=pw_d3d9_stateblock_validate(q);
 if(status)return status==PW_D3D9_SB_UNSUPPORTED?E_NOTIMPL:D3DERR_INVALIDCALL;
 switch(q->method){
 case PW_D3D9_SB_CREATE:
  if(!device || !created)return D3DERR_INVALIDCALL;
  hr=IDirect3DDevice9_CreateStateBlock(device,q->type,&result);break;
 case PW_D3D9_SB_END:
  if(!device || !created)return D3DERR_INVALIDCALL;
  hr=IDirect3DDevice9_EndStateBlock(device,&result);break;
 case PW_D3D9_SB_BEGIN:return device?IDirect3DDevice9_BeginStateBlock(device):D3DERR_INVALIDCALL;
 case PW_D3D9_SB_CAPTURE:return block?IDirect3DStateBlock9_Capture(block):D3DERR_INVALIDCALL;
 case PW_D3D9_SB_APPLY:return block?IDirect3DStateBlock9_Apply(block):D3DERR_INVALIDCALL;
 default:return E_NOTIMPL;
 }
 if(FAILED(hr)){if(result)IDirect3DStateBlock9_Release(result);return hr;}
 if(!result)return E_FAIL;
 *created=result;return hr;
}
