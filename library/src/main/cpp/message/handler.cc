/*
 * Copyright (C) 2019 Trinity. All rights reserved.
 * Copyright (C) 2019 Wang LianJie <wlanjie888@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

//
// Created by wlanjie on 2019/4/13.
//

#include "handler.h"

namespace trinity {

Handler::Handler(MessageQueue* queue) {
    this->mQueue = queue;
}

Handler::~Handler() {
}

int Handler::PostMessage(Message *msg) {
    msg->handler_ = this;
    return mQueue->EnqueueMessage(msg);
}

int Handler::GetQueueSize() {
    return mQueue->Size();
}

}  // namespace trinity
