/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/**
 * Copyright (C) 2014 Regents of the University of California.
 * See COPYING for copyright and distribution information.
 */

#include "../ndn-handle/tcp-bulk-insert-handle.hpp"

#include "sqlite-fixture.hpp"
#include "dataset-fixtures.hpp"

#include <boost/test/unit_test.hpp>

namespace repo {
namespace tests {

BOOST_AUTO_TEST_SUITE(TcpBulkInsertHandle)

class TcpClient
{
public:
  TcpClient()
    : socket(ioService)
  {
  }

  void
  start(const std::string& host, const std::string& port)
  {
    using namespace boost::asio;

    ip::tcp::resolver resolver(ioService);
    ip::tcp::resolver::query query(host, port);

    ip::tcp::resolver::iterator endpoint = resolver.resolve(query);
    ip::tcp::resolver::iterator end;

    if (endpoint == end)
      BOOST_FAIL("Cannot resolve [" + host + ":" + port + "]");

    ip::tcp::endpoint serverEndpoint = *endpoint;

    socket.async_connect(serverEndpoint,
                         bind(&TcpClient::onSuccessfullConnect, this, _1));
  }

  virtual void
  onSuccessfullConnect(const boost::system::error_code& error)
  {
    if (error)
      {
        BOOST_FAIL("TCP connection aborted");
        return;
      }
  }

public:
  boost::asio::io_service ioService;
  boost::asio::ip::tcp::socket socket;
};

template<class Dataset>
class TcpBulkInsertFixture : public TcpClient,
                             public SqliteFixture,
                             public Dataset
{
public:
  TcpBulkInsertFixture()
    : scheduler(ioService)
    , bulkInserter(ioService, *handle)
  {
    guardEvent = scheduler.scheduleEvent(ndn::time::seconds(2),
                                         bind(&TcpBulkInsertFixture::fail, this, "Test timed out"));
  }

  virtual void
  onSuccessfullConnect(const boost::system::error_code& error)
  {
    TcpClient::onSuccessfullConnect(error);

    // This value may need to be adjusted if some dataset exceeds 100k
    socket.set_option(boost::asio::socket_base::send_buffer_size(100000));

    // Initially I wrote the following to use scatter-gather approach (using
    // std::vector<const_buffer> and a single socket.async_send operation). Unfortunately, as
    // described in http://www.boost.org/doc/libs/1_48_0/doc/html/boost_asio/overview/implementation.html,
    // scatter-gather is limited to at most `min(64,IOV_MAX)` buffers to be transmitted
    // in a single operation
    for (typename Dataset::DataContainer::iterator i = this->data.begin();
         i != this->data.end(); ++i) {

      socket.async_send(boost::asio::buffer((*i)->wireEncode().wire(),  (*i)->wireEncode().size()),
                        bind(&TcpBulkInsertFixture::onSendFinished, this, _1, false));
    }

    socket.async_send(boost::asio::buffer(static_cast<const uint8_t*>(0), 0),
                      bind(&TcpBulkInsertFixture::onSendFinished, this, _1, true));
  }

  void
  onSendFinished(const boost::system::error_code& error, bool isFinal)
  {
    if (error) {
      BOOST_FAIL("TCP connection aborted");
      return;
    }

    if (isFinal) {
      scheduler.cancelEvent(guardEvent);

      // In case there are some outstanding handlers
      // ioService.post(bind(&TcpBulkInsertFixture::stop, this));
      scheduler.scheduleEvent(ndn::time::seconds(1),
                              bind(&TcpBulkInsertFixture::stop, this));
    }
  }

  void
  fail(const std::string& info)
  {
    ioService.stop();
    BOOST_FAIL(info);
  }

  void
  stop()
  {
    // Terminate test
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both);
    socket.close();

    bulkInserter.stop();
    // may be ioService.stop() as well
  }

public:
  Scheduler scheduler;
  ndn::EventId guardEvent;
  repo::TcpBulkInsertHandle bulkInserter;
};


BOOST_FIXTURE_TEST_CASE_TEMPLATE(BulkInsertAndRead, T, DatasetFixtures, TcpBulkInsertFixture<T>)
{
  BOOST_TEST_MESSAGE(T::getName());
  // BOOST_CHECK_EQUAL(this->handle->size(), 1);

  // start bulk inserter
  this->bulkInserter.listen("localhost", "17376");

  // start test
  this->start("localhost", "17376");

  // actually run the test
  this->ioService.run();

  BOOST_CHECK_EQUAL(this->handle->size(), this->data.size());

  // Read (all items should exist)
  for (typename T::InterestContainer::iterator i = this->interests.begin();
       i != this->interests.end(); ++i) {
    ndn::Data retrievedData;
    BOOST_REQUIRE_EQUAL(this->handle->readData(i->first, retrievedData), true);
    BOOST_CHECK_EQUAL(retrievedData, *i->second);
  }
}


BOOST_AUTO_TEST_SUITE_END()

} // namespace repo
} // namespace tests