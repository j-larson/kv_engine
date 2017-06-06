/* -*- MODE: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2017 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

/*
 * Testsuite for Item class in ep-engine.
 */

#include "item.h"
#include "test_helpers.h"

#include <gtest/gtest.h>
#include <memcached/protocol_binary.h>
#include <platform/make_unique.h>

class ItemTest : public ::testing::Test {
public:

    SingleThreadedRCPtr<Item> item;
};

TEST_F(ItemTest, getAndSetCachedDataType) {
    std::string valueData = R"(raw data)";
    item = std::make_unique<Item>(
            makeStoredDocKey("key"),
            0,
            0,
            valueData.c_str(),
            valueData.size());

    // Item was created with no extended meta data so datatype should be
    // the default PROTOCOL_BINARY_RAW_BYTES
    EXPECT_EQ(PROTOCOL_BINARY_RAW_BYTES, item->getDataType());

    // We can set still set the cached datatype
    item->setDataType(PROTOCOL_BINARY_DATATYPE_SNAPPY);
    // Check that the datatype equals what we set it to
    EXPECT_EQ(PROTOCOL_BINARY_DATATYPE_SNAPPY, item->getDataType());

    uint8_t ext_meta[EXT_META_LEN] = {PROTOCOL_BINARY_DATATYPE_JSON};
    std::string jsonValueData = R"({"json":"yes"})";

    auto blob = Blob::New(jsonValueData.c_str(),
                          jsonValueData.size(),
                          ext_meta,
                          sizeof(ext_meta));

    // Replace the item's blob with one that contains extended meta data
    item->setValue(blob);

    // Expect the cached datatype to be equal to the one in the new blob
    EXPECT_EQ(PROTOCOL_BINARY_DATATYPE_JSON,
              (PROTOCOL_BINARY_DATATYPE_JSON & item->getDataType()));
}
