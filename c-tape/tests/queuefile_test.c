/*
 * Copyright (C) 2012 Square, Inc.
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
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/queue.h>

#include "minunit.h"

#include "../fileio.h"
#include "../logutil.h"
#include "../queuefile.h"
#include "../types.h"

/**
 * Takes up 33401 bytes in the queue (N*(N+1)/2+4*N). Picked 254 instead of
 * 255 so that the number of bytes isn't a multiple of 4.
 */
#define N 254
static byte* values[N];

#define TEST_QUEUE_FILENAME "test.queue"
static QueueFile* queue;
int tests_run = 0;


typedef STAILQ_HEAD(listHead_t, listEntry_t) listHead;
static void _assertPeekCompare(QueueFile *queue, byte* data, uint32_t length);
static void _assertPeekCompareRemove(QueueFile *queue, byte* data,
    uint32_t length);
static void _assertPeekCompareRemoveDequeue(QueueFile *queue,
    listHead *expectqueue);

static void mu_setup() {
  int i;
  for (i = 0; i < N; i++) {
    values[i] = malloc((size_t)i);
    // Example: values[3] = { 3, 2, 1 }
    int ii;
    for (ii = 0; ii < i; ii++) values[i][ii] = (byte) (i - ii);
  }

  // Default case is start with a clean queue.
  remove(TEST_QUEUE_FILENAME);
  queue = QueueFile_new(TEST_QUEUE_FILENAME);
  mu_assert_notnull(queue);
}

static void mu_teardown() {
  QueueFile_close(queue);
  free(queue);

  int i;
  for (i = 0; i < N; i++) {
    free(values[i]);
  }
}

static void testSimpleAddOneElement() {
  byte* expected = values[253];
  QueueFile_add(queue, expected, 0, 253);
  _assertPeekCompare(queue, expected, 253);
}

static void testAddOneElement() {
  byte* expected = values[253];
  QueueFile_add(queue, expected, 0, 253);
  _assertPeekCompare(queue, expected, 253);
  QueueFile_close(queue);
  free(queue);
  queue = QueueFile_new(TEST_QUEUE_FILENAME);
  _assertPeekCompare(queue, expected, 253);
}


// stuct for test queue.
struct listEntry_t {
  byte *data;
  uint32_t length;
  STAILQ_ENTRY(listEntry_t) next_entry;
};

struct listEntry_t* listEntry_new(byte *argdata, uint32_t arglen) {
  struct listEntry_t* retval = malloc(sizeof(struct listEntry_t));
  mu_assert_notnull(retval);
  retval->data = argdata;
  retval->length = arglen;
  return retval;
};

static void testAddAndRemoveElements() {
  QueueFile_close(queue);
  free(queue);

  time_t start = time(NULL);

  listHead expect = STAILQ_HEAD_INITIALIZER(expect);
  struct listEntry_t *entry;

  int round;
  for (round = 0; round < 5; round++) {
    queue = QueueFile_new(TEST_QUEUE_FILENAME);
    int i;
    for (i = 0; i < N; i++) {
      QueueFile_add(queue, values[i], 0, (uint32_t)i);
      entry = listEntry_new(values[i], (uint32_t)i);
      STAILQ_INSERT_TAIL(&expect, entry, next_entry);
    }

    // Leave N elements in round N, 15 total for 5 rounds. Removing all the
    // elements would be like starting with an empty queue.
    for (i = 0; i < N - round - 1; i++) {
      _assertPeekCompareRemoveDequeue(queue, &expect);
    }
    QueueFile_close(queue);
    free(queue);
  }

  // Remove and validate remaining 15 elements.
  queue = QueueFile_new(TEST_QUEUE_FILENAME);
  mu_assert(QueueFile_size(queue) == 15);

  int expectCount = 0;
  STAILQ_FOREACH(entry, &expect, next_entry)
    ++expectCount;

  mu_assert(expectCount == 15);

  while (!STAILQ_EMPTY(&expect)) {
    _assertPeekCompareRemoveDequeue(queue, &expect);
  }

  time_t stop = time(NULL);

  LOG(LINFO, "Ran in %lf seconds.", difftime(stop, start));
}

/** Tests queue expansion when the data crosses EOF. */
static void testSplitExpansion() {
  // This should result in 3560 bytes.
  int max = 80;

  listHead expect = STAILQ_HEAD_INITIALIZER(expect);
  struct listEntry_t *entry;

  int i;
  for (i = 0; i < max; i++) {
    QueueFile_add(queue, values[i], 0, (uint32_t)i);
    entry = listEntry_new(values[i], (uint32_t)i);
    STAILQ_INSERT_TAIL(&expect, entry, next_entry);
  }


  // Remove all but 1.
  for (i = 1; i < max; i++) {
    _assertPeekCompareRemoveDequeue(queue, &expect);
  }

  uint32_t flen1 = FileIo_getLength(_for_testing_QueueFile_get_fhandle(queue));

  // This should wrap around before expanding.
  for (i = 0; i < N; i++) {
    QueueFile_add(queue, values[i], 0, (uint32_t)i);
    entry = listEntry_new(values[i], (uint32_t)i);
    STAILQ_INSERT_TAIL(&expect, entry, next_entry);
  }

  while (!STAILQ_EMPTY(&expect)) {
    _assertPeekCompareRemoveDequeue(queue, &expect);
  }

  uint32_t flen2 = FileIo_getLength(_for_testing_QueueFile_get_fhandle(queue));
  mu_assertm(flen1 == flen2, "file size should remain same");
}

/**
 * Exercise a bug where wrapped elements were getting corrupted when the
 * QueueFile was forced to expand in size and a portion of the final Element
 * had been wrapped into space at the beginning of the file - if multiple
 * Elements have been written to empty buffer space at the start does the
 * expansion correctly update all their positions?
 */
static void testFileExpansionCorrectlyMovesElements() {

  // Create test data - 1k blocks marked consecutively 1, 2, 3, 4 and 5.
  int valuesLength = 5;
  uint32_t valuesSize = 1024;
  byte* values[valuesLength];
  int blockNum;
  for (blockNum = 0; blockNum < valuesLength; blockNum++) {
    values[blockNum] = malloc((size_t)valuesSize);
    uint32_t i;
    for (i = 0; i < valuesSize; i++) {
      values[blockNum][i] = (byte) (blockNum + 1);
    }
  }

  // smaller data elements
  int smallerLength = 3;
  uint32_t smallerSize = 256;
  byte* smaller[smallerLength];
  for (blockNum = 0; blockNum < smallerLength; blockNum++) {
    smaller[blockNum] = malloc((size_t)smallerSize);
    uint32_t i;
    for (i = 0; i < smallerSize; i++) {
      smaller[blockNum][i] = (byte) (blockNum + 6);
    }
  }

  // First, add the first two blocks to the queue, remove one leaving a
  // 1K space at the beginning of the buffer.
  mu_assert(QueueFile_add(queue, values[0], 0, valuesSize));
  mu_assert(QueueFile_add(queue, values[1], 0, valuesSize));
  mu_assert(QueueFile_remove(queue));

  // The trailing end of block "4" will be wrapped to the start of the buffer.
  mu_assert(QueueFile_add(queue, values[2], 0, valuesSize));
  mu_assert(QueueFile_add(queue, values[3], 0, valuesSize));

  // Now fill in some space with smaller blocks, none of which will cause
  // an expansion.
  mu_assert(QueueFile_add(queue, smaller[0], 0, smallerSize));
  mu_assert(QueueFile_add(queue, smaller[1], 0, smallerSize));
  mu_assert(QueueFile_add(queue, smaller[2], 0, smallerSize));

  // Cause buffer to expand as there isn't space between the end of the
  // smaller block "8" and the start of block "2".  Internally the queue
  // should cause all of tbe smaller blocks, and the trailing end of
  // block "5" to be moved to the end of the file.
  mu_assert(QueueFile_add(queue, values[4], 0, valuesSize));

  uint32_t expectedBlockLen = 6;
  byte expectedBlockNumbers[] = {2, 3, 4, 6, 7, 8,};

  // Make sure values are not corrupted, specifically block "4" that wasn't
  // being made contiguous in the version with the bug.
  uint32_t i;
  for (i = 0; i < expectedBlockLen; i++) {
    byte expectedBlockNumber = expectedBlockNumbers[i];
    uint32_t length;
    byte* value = QueueFile_peek(queue, &length);
    mu_assert(QueueFile_remove(queue));

    uint32_t j;
    for (j = 0; j < length; j++) {
      mu_assert(value[j] == expectedBlockNumber);
    }
    free(value);
  }
}


static void testFailedAdd() {
  mu_assert(QueueFile_add(queue, values[253], 0, 253));
  _for_testing_FileIo_failAllWrites((int)1);
  mu_assert(!QueueFile_add(queue, values[252], 0, 252));
  _for_testing_FileIo_failAllWrites(false);

  // Allow a subsequent add to succeed.
  mu_assert(QueueFile_add(queue, values[251], 0, 251));

  QueueFile_close(queue);
  free(queue);
  queue = QueueFile_new(TEST_QUEUE_FILENAME);

  mu_assert(QueueFile_size(queue) == 2);
  _assertPeekCompareRemove(queue, values[253], 253);
  _assertPeekCompareRemove(queue, values[251], 251);
}

static void testFailedRemoval() {
  mu_assert(QueueFile_add(queue, values[253], 0, 253));
  _for_testing_FileIo_failAllWrites(true);
  mu_assert(!QueueFile_remove(queue));
  _for_testing_FileIo_failAllWrites(false);

  QueueFile_close(queue);
  free(queue);
  queue = QueueFile_new(TEST_QUEUE_FILENAME);

  mu_assert(QueueFile_size(queue) == 1);
  _assertPeekCompareRemove(queue, values[253], 253);
  mu_assert(QueueFile_add(queue, values[99], 0, 99));
  _assertPeekCompareRemove(queue, values[99], 99);
}

static void testFailedExpansion() {
  mu_assert(QueueFile_add(queue, values[253], 0, 253));
  _for_testing_FileIo_failAllWrites(true);
  byte bigbuf[8000];
  mu_assert(!QueueFile_add(queue, bigbuf, 0, 8000));
  _for_testing_FileIo_failAllWrites(false);

  QueueFile_close(queue);
  free(queue);
  queue = QueueFile_new(TEST_QUEUE_FILENAME);

  mu_assert(QueueFile_size(queue) == 1);

  _assertPeekCompare(queue, values[253], 253);
  mu_assert(4096 == FileIo_getLength(_for_testing_QueueFile_get_fhandle(queue)));
  mu_assert(QueueFile_add(queue, values[99], 0, 99));
  _assertPeekCompareRemove(queue, values[253], 253);
  _assertPeekCompareRemove(queue, values[99], 99);
}


int main() {
  LOG_SETDEBUGFAILLEVEL_WARN;
  mu_run_test(testSimpleAddOneElement);
  mu_run_test(testAddOneElement);
  mu_run_test(testAddAndRemoveElements);
  mu_run_test(testSplitExpansion);
  mu_run_test(testFileExpansionCorrectlyMovesElements);
  mu_run_test(testFailedAdd);
  mu_run_test(testFailedRemoval);
  mu_run_test(testFailedExpansion);

  printf("%d tests passed.\n", tests_run);
  return 0;
}



// ------------- utility methods ---------------

static void _assertPeekCompare(QueueFile *queue, byte* data, uint32_t length) {
  uint32_t qlength;
  byte* actual = QueueFile_peek(queue, &qlength);
  mu_assert(qlength == length);
  mu_assert_memcmp(data, actual, length);
  free(actual);
}

static void _assertPeekCompareRemove(QueueFile *queue, byte* data,
    uint32_t length) {
  _assertPeekCompare(queue, data, length);
  mu_assert(QueueFile_remove(queue));
}

static void _assertPeekCompareRemoveDequeue(QueueFile *queue,
    struct listHead_t *expectqueue) {
  struct listEntry_t *entry = STAILQ_FIRST(expectqueue);
  mu_assert_notnull(entry);
  _assertPeekCompareRemove(queue, entry->data, entry->length);
  STAILQ_REMOVE_HEAD(expectqueue, next_entry);
  free(entry);
}

