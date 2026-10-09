/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef PW_VK_PRESENT_PE_H
#define PW_VK_PRESENT_PE_H
/* Include after vulkan_loader.h. Raw thunk wrapper shared by both PE ABIs. */
NTSTATUS pw_vk_present_call(unsigned int,void *);
void pw_vk_present_forget(void);
#endif
