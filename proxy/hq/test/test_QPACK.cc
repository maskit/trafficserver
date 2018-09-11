/** @file
 *
 *  A brief file description
 *
 *  @section license License
 *
 *  Licensed to the Apache Software Foundation (ASF) under one
 *  or more contributor license agreements.  See the NOTICE file
 *  distributed with this work for additional information
 *  regarding copyright ownership.  The ASF licenses this file
 *  to you under the Apache License, Version 2.0 (the
 *  "License"); you may not use this file except in compliance
 *  with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "catch.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include "XPACK.h"
#include "QPACK.h"
#include "HTTP.h"
#include "../../iocore/net/quic/Mock.h"

// Declared in main_qpack.cc
extern char qifdir[256];
extern int tablesize;
extern int streams;
extern int ackmode;
extern char appname[256];

constexpr int ACK_MODE_IMMEDIATE = 1;
constexpr int ACK_MODE_NONE      = 0;

class TestQUICConnection : public MockQUICConnection
{
};

class QUICApplicationDriver
{
public:
  QUICApplicationDriver() {}

  QUICConnection *
  get_connection()
  {
    return &this->_connection;
  }

private:
  TestQUICConnection _connection;
};

class TestQUICStream : public QUICStream
{
public:
  TestQUICStream(QUICStreamId sid) : QUICStream(new MockQUICRTTProvider(), new MockQUICConnectionInfoProvider(), sid, 65536, 65536)
  {
  }

  void
  write(const uint8_t *buf, size_t buf_len, QUICOffset offset, bool last)
  {
    this->_write_to_read_vio(offset, buf, buf_len, last);
    this->_signal_read_event();
  }

  size_t
  read(uint8_t *buf, size_t buf_len)
  {
    this->_signal_write_event();
    IOBufferReader *reader = this->_write_vio.get_reader();
    return reader->read(buf, buf_len);
  }
};

class TestQPACKEventHandler : public Continuation
{
public:
  TestQPACKEventHandler() : Continuation() { SET_HANDLER(&TestQPACKEventHandler::event_handler); }

  int
  event_handler(int event, Event *data)
  {
    this->_event = event;
    return 0;
  }

  int
  last_event()
  {
    return this->_event;
  }

private:
  int _event = 0;
};

static int
load_qif_file(const char *filename, HTTPHdr **headers)
{
  HTTPHdr *hdr = nullptr;
  int n        = 0;
  std::ifstream ifs(filename);
  std::string line;

  while (std::getline(ifs, line)) {
    if (line.empty()) {
      if (hdr) {
        headers[n++] = hdr;
        hdr          = nullptr;
      } else {
        continue;
      }
    } else if (line.at(0) == '#') {
      continue;
    } else {
      if (!hdr) {
        hdr = new HTTPHdr();
        hdr->create(HTTP_TYPE_REQUEST);
      }
      auto tab   = line.find_first_of('\t');
      auto name  = line.substr(0, tab);
      auto value = line.substr(tab + 1);
      hdr->value_set(name.c_str(), tab, value.c_str(), line.length() - tab - 1);
    }
  }
  if (hdr) {
    headers[n++] = hdr;
  }

  return n;
}

void
output_encoder_stream_data(FILE *fd, TestQUICStream *stream)
{
  uint8_t buf[1024];

  // Write StreamId (0)
  uint64_t stream_id = 0;
  fwrite(reinterpret_cast<uint8_t *>(&stream_id), 8, 1, fd);

  // Skip 32 bits for Legnth
  fseek(fd, 4, SEEK_CUR);

  // Write QPACKData
  uint64_t total, nread;
  total = 0;
  while ((nread = stream->read(buf, sizeof(buf))) > 0) {
    fwrite(buf, nread, 1, fd);
    total += nread;
  }

  // Back to the posistion for Length
  fseek(fd, -(total + 4), SEEK_CUR);

  // Write Length
  uint32_t len = htobe32(total);
  fwrite(reinterpret_cast<uint8_t *>(&len), 4, 1, fd);

  // Back to the tail
  fseek(fd, 0, SEEK_END);
}

void
output_encoded_data(FILE *fd, uint64_t stream_id, IOBufferReader *header_block_reader)
{
  uint8_t buf[1024];

  // Write StreamId
  stream_id = htobe64(stream_id);
  fwrite(reinterpret_cast<uint8_t *>(&stream_id), 8, 1, fd);

  // Skip 32 bits for Legnth
  fseek(fd, 4, SEEK_CUR);

  // Write QPACKData
  int64_t total, nread;
  total = 0;
  while ((nread = header_block_reader->read(buf, sizeof(buf))) > 0) {
    fwrite(buf, nread, 1, fd);
    total += nread;
  }

  // Back to the posistion for Length
  fseek(fd, -(total + 4), SEEK_CUR);

  // Write Length
  uint32_t len = htobe32(total);
  fwrite(reinterpret_cast<uint8_t *>(&len), 4, 1, fd);

  // Back to the tail
  fseek(fd, 0, SEEK_END);
}

void
output_decoded_headers(FILE *fd, HTTPHdr **headers, uint64_t n)
{
  for (uint64_t i = 0; i < n; ++i) {
    HTTPHdr *header_set = headers[i];
    if (!header_set) {
      continue;
    }
    fprintf(fd, "# stream %llu\n", i + 1);
    MIMEFieldIter field_iter;
    for (MIMEField *field = header_set->iter_get_first(&field_iter); field != nullptr;
         field            = header_set->iter_get_next(&field_iter)) {
      int name_len;
      int value_len;
      const char *name  = field->name_get(&name_len);
      const char *value = field->value_get(&value_len);
      fprintf(fd, "%.*s\t%.*s\n", name_len, name, value_len, value);
    }
    fprintf(fd, "\n");
  }
}

static int
read_block(FILE *fd, uint64_t &stream_id, uint8_t **head, uint32_t &block_len)
{
  size_t len;

  // Read Stream ID
  len = fread(&stream_id, 1, 8, fd);
  if (len != 8) {
    return -1;
  }
  stream_id = be64toh(stream_id);

  // Read Length
  len = fread(&block_len, 1, 4, fd);
  if (len != 4) {
    return -1;
  }
  block_len = be32toh(block_len);

  // Set the head of block
  *head = reinterpret_cast<uint8_t *>(ats_malloc(block_len));
  len   = fread(*head, 1, block_len, fd);
  if (len != block_len) {
    ats_free(*head);
    return -1;
  }

  return 0;
}

void
acknowledge_header_block(TestQUICStream *stream, uint64_t stream_id)
{
  uint8_t buf[128];
  int buf_len = 0;

  buf[0]  = 0x80;
  int ret = xpack_encode_integer(buf, buf + sizeof(buf), stream_id, 7);
  stream->write(buf, ret, 0, stream_id);
}

static int
test_encode(const char *qif_file, int dts, int mbs, int am)
{
  int ret = 0;

  char output_filename[256];
  sprintf(output_filename, "%s.ats.%d.%d.%d", qif_file, dts, mbs, am);
  FILE *fd = fopen(output_filename, "w");

  HTTPHdr *requests[256] = {nullptr};
  int n_requests         = load_qif_file(qif_file, requests);

  QUICApplicationDriver driver;
  QPACK *qpack                   = new QPACK(driver.get_connection(), dts);
  TestQUICStream *encoder_stream = new TestQUICStream(0);
  TestQUICStream *decoder_stream = new TestQUICStream(9999);
  qpack->set_stream(encoder_stream);
  qpack->set_stream(decoder_stream);

  uint64_t stream_id                  = 1;
  MIOBuffer *header_block             = new_MIOBuffer();
  IOBufferReader *header_block_reader = header_block->alloc_reader();
  for (int i = 0; i < n_requests; ++i) {
    HTTPHdr *hdr = requests[i];
    ret          = qpack->encode(stream_id, *hdr, header_block);
    if (ret < 0) {
      break;
    }

    output_encoder_stream_data(fd, encoder_stream);
    output_encoded_data(fd, stream_id, header_block_reader);

    if (am == ACK_MODE_IMMEDIATE) {
      acknowledge_header_block(decoder_stream, stream_id);
    }

    ++stream_id;
  }

  fflush(fd);
  fclose(fd);

  return ret;
}

static int
test_decode(const char *qif_file, int dts, int mbs, int am, const char *app_name)
{
  int ret = 0;

  char data_filename[256];
  sprintf(data_filename, "%s.%s.%d.%d.%d", qif_file, app_name, dts, mbs, am);
  FILE *fd_in = fopen(data_filename, "r");

  char output_filename[256];
  sprintf(output_filename, "%s.decoded", data_filename);
  FILE *fd_out = fopen(output_filename, "w");

  HTTPHdr *requests[256];
  int n_requests = load_qif_file(qif_file, requests);

  TestQPACKEventHandler *event_handler = new TestQPACKEventHandler();

  QUICApplicationDriver driver;
  QPACK *qpack                   = new QPACK(driver.get_connection(), dts);
  TestQUICStream *encoder_stream = new TestQUICStream(0);
  qpack->set_stream(encoder_stream);

  int offset     = 0;
  uint8_t *block = nullptr;
  uint32_t block_len;
  int read_len = 0;

  uint64_t stream_id        = 1;
  HTTPHdr *header_sets[256] = {nullptr};
  int n_headers             = 0;
  while ((read_len = read_block(fd_in, stream_id, &block, block_len)) >= 0) {
    if (stream_id == encoder_stream->id()) {
      encoder_stream->write(block, block_len, offset, false);
      offset += block_len;
    } else {
      if (!header_sets[stream_id - 1]) {
        header_sets[stream_id - 1] = new HTTPHdr();
        header_sets[stream_id - 1]->create(HTTP_TYPE_REQUEST);
        ++n_headers;
      }
      qpack->decode(stream_id, block, block_len, *header_sets[stream_id - 1], event_handler, eventProcessor.all_ethreads[0]);
    }
    ats_free(block);
  }

  if (!feof(fd_in)) {
    return -1;
  }

  sleep(3);

  CHECK(event_handler->last_event() == QPACK_EVENT_DECODE_COMPLETE);

  output_decoded_headers(fd_out, header_sets, n_headers);

  for (unsigned int i = 0; i < countof(header_sets); ++i) {
    if (header_sets[i]) {
      header_sets[i]->destroy();
      delete header_sets[i];
    }
  }

  return ret;
}

TEST_CASE("Encoding", "[qpack]")
{
  struct dirent *d;
  DIR *dir = opendir(qifdir);

  if (dir == nullptr) {
    return;
  }

  struct stat st;
  char qif_file[PATH_MAX + 1] = "";
  strcat(qif_file, qifdir);

  while ((d = readdir(dir)) != nullptr) {
    char section_name[128];
    sprintf(section_name, "%s: DTS=%d, MBS=%d, AM=%d", d->d_name, tablesize, streams, ackmode);
    SECTION(section_name)
    {
      qif_file[strlen(qifdir)]     = '/';
      qif_file[strlen(qifdir) + 1] = '\0';
      ink_strlcat(qif_file, d->d_name, sizeof(qif_file));
      stat(qif_file, &st);
      if (S_ISREG(st.st_mode) && strstr(d->d_name, ".qif") == (d->d_name + (strlen(d->d_name) - 4))) {
        CHECK(test_encode(qif_file, tablesize, streams, ackmode) == 0);
      }
    }
  }
}

TEST_CASE("Decoding", "[qpack]")
{
  struct dirent *d;
  DIR *dir = opendir(qifdir);

  if (dir == nullptr) {
    return;
  }

  struct stat st;
  char qif_file[PATH_MAX + 1] = "";
  strcat(qif_file, qifdir);

  while ((d = readdir(dir)) != nullptr) {
    char section_name[128];
    sprintf(section_name, "%s: DTS=%d, MBS=%d, AM=%d, APP=%s", d->d_name, tablesize, streams, ackmode, appname);
    SECTION(section_name)
    {
      qif_file[strlen(qifdir)]     = '/';
      qif_file[strlen(qifdir) + 1] = '\0';
      ink_strlcat(qif_file, d->d_name, sizeof(qif_file));
      stat(qif_file, &st);
      if (S_ISREG(st.st_mode) && strstr(d->d_name, ".qif") == (d->d_name + (strlen(d->d_name) - 4))) {
        CHECK(test_decode(qif_file, tablesize, streams, ackmode, appname) == 0);
      }
    }
  }
}
