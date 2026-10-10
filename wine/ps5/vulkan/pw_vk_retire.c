/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "pw_vk_retire.h"
int pw_vk_retirement_add(struct pw_vk_retirement *q,void *object,void *(*allocate)(size_t))
{
 struct pw_vk_retired *node;
 if(!object)return 1;
 if(!(node=allocate(sizeof(*node))))return 0;
 node->object=object;node->next=q->head;q->head=node;return 1;
}
void pw_vk_retirement_drain(struct pw_vk_retirement *q,void (*release_object)(void *),void (*release_node)(void *))
{
 struct pw_vk_retired *node=q->head,*next;q->head=NULL;
 while(node){next=node->next;release_object(node->object);release_node(node);node=next;}
}
