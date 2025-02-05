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

#ifndef TRINITY_HANDLER_H
#define TRINITY_HANDLER_H

#include "handler.h"
#include "message_queue.h"

namespace trinity {

class Handler {
 private:
    MessageQueue* mQueue;

 public:
    explicit Handler(MessageQueue* mQueue);
    ~Handler();

    int PostMessage(Message *msg);
    int GetQueueSize();
    virtual void HandleMessage(Message *msg){}
};

}  // namespace trinity

#endif  // TRINITY_HANDLER_H
