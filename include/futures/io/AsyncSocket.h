#pragma once

#include <futures/TcpStream.h>
#include <futures/io/WaitHandleBase.h>
#include <futures/core/SocketAddress.h>

namespace futures {
namespace io {

class SocketChannel : public IOObject {
    enum State {
        INITED,
        CONNECTING,
        CONNECTED,
        CLOSED,
    };

    enum ReadResult {
        READ_EOF = 0,
        READ_WOULDBLOCK = -1,
        READ_ERROR = -2,
    };

    enum ShutdownFlags {
        SHUT_WRITE_PENDING = 0x01,
        SHUT_WRITE = 0x02,
        SHUT_READ = 0x04,
    };
public:
    using Ptr = std::shared_ptr<SocketChannel>;

    SocketChannel(EventExecutor *ev)
        : IOObject(ev), rio_(ev->getLoop()), wio_(ev->getLoop())
    {
        rio_.set<SocketChannel, &SocketChannel::onEvent>(this);
        wio_.set<SocketChannel, &SocketChannel::onEvent>(this);
    }

    SocketChannel(EventExecutor *ev, tcp::Socket socket,
            const folly::SocketAddress& peer = folly::SocketAddress())
        : IOObject(ev),
          socket_(std::move(socket)), peer_addr_(peer), s_(CONNECTED),
          rio_(ev->getLoop()), wio_(ev->getLoop())
    {
        assert(socket_.fd() != -1);

        rio_.set<SocketChannel, &SocketChannel::onEvent>(this);
        wio_.set<SocketChannel, &SocketChannel::onEvent>(this);
        wio_.set(socket_.fd(), ev::WRITE);
        rio_.set(socket_.fd(), ev::READ);
    }

    struct ReaderCompletionToken : public io::CompletionToken {
        std::error_code ec;
        // std::unique_ptr<folly::IOBuf> buf_;
        using Item = std::unique_ptr<folly::IOBuf>;

        ReaderCompletionToken()
            : io::CompletionToken(IOObject::OpRead) {
        }

        void onCancel(CancelReason r) override {
        }

        virtual void readEof() {
            notifyDone();
        }

        virtual void readError(std::error_code ec) {
            this->ec = ec;
            notifyDone();
        }

        virtual void dataReady(ssize_t size) = 0;
        virtual void prepareBuffer(void **buf, size_t *data) = 0;

        ~ReaderCompletionToken() {
            cleanup(CancelReason::UserCancel);
        }
    };

    struct WriterCompletionToken : public io::CompletionToken {
        std::error_code ec;
        ssize_t written = 0;

        WriterCompletionToken(std::unique_ptr<folly::IOBuf> buf)
            : io::CompletionToken(IOObject::OpWrite), buf_(std::move(buf)) {
            size_t chain_len = buf_->countChainElements();
            if (!chain_len)
                throw std::invalid_argument("empty chain");
            if (chain_len <= kMaxIovLen) {
                iovec_len_ = buf_->fillIov(small_vec_, kMaxIovLen);
                piovec_ = small_vec_;
            } else {
                vec_ = buf_->getIov();
                piovec_ = vec_.data();
                iovec_len_ = vec_.size();
            }
        }

        bool doWrite(std::error_code &ec) {
            auto s = static_cast<SocketChannel*>(getIOObject());
            size_t countWritten;
            size_t partialWritten;
            ssize_t totalWritten = s->performWrite(piovec_, iovec_len_,
                    &countWritten, &partialWritten, ec);
            if (ec) {
                this->ec = ec;
                return true;
            } else {
                written += totalWritten;
                if (countWritten == iovec_len_) {
                    // done
                    return true;
                } else {
                    piovec_ += countWritten;
                    iovec_len_ -= countWritten;
                    assert(iovec_len_ > 0);
                    piovec_[0].iov_base = reinterpret_cast<char*>(piovec_[0].iov_base) + partialWritten;
                    piovec_[0].iov_len -= partialWritten;
                    return false;
                }
            }
        }

        void onCancel(CancelReason r) override {
        }

        Poll<ssize_t> poll() {
            switch (getState()) {
            case STARTED:
                park();
                return Poll<ssize_t>(not_ready);
            case DONE:
                if (ec)
                    return Poll<ssize_t>(IOError("writev", ec));
                else
                    return makePollReady(written);
            case CANCELLED:
                return Poll<ssize_t>(FutureCancelledException());
            }
        }

    protected:
        ~WriterCompletionToken() {
            cleanup(CancelReason::UserCancel);
        }

        std::unique_ptr<folly::IOBuf> buf_;

        static const size_t kMaxIovLen = 32;
        iovec small_vec_[kMaxIovLen];
        std::vector<iovec> vec_;

        iovec *piovec_ = nullptr;
        size_t iovec_len_ = 0;
    };

    struct ConnectCompletionToken : public io::CompletionToken {
        std::error_code ec;

        ConnectCompletionToken(SocketChannel *ctx)
            : io::CompletionToken(IOObject::OpConnect)
        {
        }

        void onCancel(CancelReason r) override {
        }

        Poll<folly::Unit> poll() {
            switch (getState()) {
            case STARTED:
                park();
                return Poll<folly::Unit>(not_ready);
            case DONE:
                if (ec)
                    return Poll<folly::Unit>(IOError("connect", ec));
                else
                    return makePollReady(folly::unit);
            case CANCELLED:
                return Poll<folly::Unit>(FutureCancelledException());
            }
        }

        ~ConnectCompletionToken() {
            cleanup(CancelReason::UserCancel);
        }
    };

    io::intrusive_ptr<ConnectCompletionToken> doConnect(const folly::SocketAddress &addr) {
        if (s_ != INITED)
            throw IOError("Already connecting");
        peer_addr_ = addr;
        io::intrusive_ptr<ConnectCompletionToken> tok(new ConnectCompletionToken(this));
        if (startConnect(tok->ec)) {
            tok->notifyDone();
        } else {
            tok->attach(this);
        }
        return tok;
    }

    io::intrusive_ptr<ReaderCompletionToken> doRead(std::unique_ptr<ReaderCompletionToken> p) {
        if (!getPending(IOObject::OpRead).empty())
            throw IOError("Already reading");
        if (s_ == INITED)
            throw IOError("Not connecting");
        io::intrusive_ptr<ReaderCompletionToken> tok(p.release());
        if ((s_ == CLOSED) || (shutdown_flags_ & SHUT_READ)) {
            tok->ec = std::make_error_code(std::errc::connection_aborted);
            tok->notifyDone();
        } else {
            tok->attach(this);
            rio_.start();
        }
        return tok;
    }

    io::intrusive_ptr<WriterCompletionToken> doWrite(std::unique_ptr<folly::IOBuf> buf) {
        if (s_ == INITED)
            throw IOError("Not connecting");
        io::intrusive_ptr<WriterCompletionToken> tok(new WriterCompletionToken(std::move(buf)));
        if ((s_ == CLOSED)
                || (shutdown_flags_ & SHUT_WRITE_PENDING)
                || (shutdown_flags_ & SHUT_WRITE)) {
            tok->ec = std::make_error_code(std::errc::connection_aborted);
            tok->notifyDone();
        } else if (s_ == CONNECTED) {
            // TODO try send immediately
            tok->attach(this);
            wio_.start();
        }
        return tok;
    }
    io::intrusive_ptr<WriterCompletionToken> doFlush();

    bool good() const {
        return (s_ == CONNECTING || s_ == CONNECTED)
            && (shutdown_flags_ == 0);
    }

    bool startConnect(std::error_code &ec);

    ssize_t performWrite(
            const iovec* vec,
            size_t count,
            size_t* countWritten,
            size_t* partialWritten,
            std::error_code &ec);

    ssize_t performRead(ReaderCompletionToken *tok, std::error_code &ec);

    void onCancel(CancelReason r) {
        if (s_ != CLOSED)
            forceClose();
    }

    void shutdownWrite() {
        if (getPending(IOObject::OpWrite).empty()) {
            shutdownWriteNow();
            return;
        }
        shutdown_flags_ |= SHUT_WRITE_PENDING;
    }

    void shutdownWriteNow() {
        if (shutdown_flags_ & SHUT_WRITE)
            return;
        if (shutdown_flags_ & SHUT_READ) {
            cleanup(CancelReason::IOObjectShutdown);
            return;
        }
        std::error_code ec;
        switch (s_) {
        case CONNECTED:
            shutdown_flags_ |= SHUT_WRITE;
            wio_.stop();
            socket_.shutdown(::SHUT_WR, ec);
            failAllWrites();
            return;
        case CONNECTING:
            shutdown_flags_ |= SHUT_WRITE_PENDING;
            failAllWrites();
            return;
        case INITED:
            shutdown_flags_ |= SHUT_WRITE_PENDING;
            return;
        case CLOSED:
            FUTURES_LOG(WARNING) << "shutdown a closed socket";
        }
    }

private:
    tcp::Socket socket_;
    folly::SocketAddress peer_addr_;
    State s_ = INITED;
    int shutdown_flags_ = 0;
    ev::io rio_;
    ev::io wio_;

    ssize_t doAsyncRead(void* buf, size_t buflen, std::error_code &ec) {
        ssize_t r = socket_.recv(buf, buflen, 0, ec);
        if (!ec) {
            return r == 0 ? READ_EOF : r;
        } else if (ec == std::make_error_code(std::errc::operation_would_block)) {
            return READ_WOULDBLOCK;
        } else {
            return READ_ERROR;
        }
    }

    void onEvent(ev::io& watcher, int revent) {
        if (revent & ev::ERROR)
            throw std::runtime_error("syscall error");
        if (revent & ev::READ) {
            if (s_ == CONNECTED) {
                auto &reader = getPending(IOObject::OpRead);
                if (!reader.empty()) {
                    auto first = static_cast<ReaderCompletionToken*>(&reader.front());
                    std::error_code ec;
                    ssize_t ret = performRead(first, ec);
                    if (ret == READ_EOF) {
                        // todo
                        closeRead();
                    } else if (ret == READ_ERROR) {
                        cleanup(CancelReason::IOObjectShutdown);
                    } else if (ret == READ_WOULDBLOCK) {
                        // nothing
                    }
                } else {
                    rio_.stop();
                }
            }
        }
        if (revent & ev::WRITE) {
            if (s_ == CONNECTING) {
                std::error_code ec;
                bool connected = socket_.is_connected(ec);
                auto &connect = getPending(IOObject::OpConnect);
                while (!connect.empty()) {
                    auto p = static_cast<ConnectCompletionToken*>(&connect.front());
                    p->ec = ec;
                    p->notifyDone();
                }

                if (connected) {
                    s_ = CONNECTED;
                } else {
                    cleanup(CancelReason::IOObjectShutdown);
                    return;
                }
            }
            if (s_ == CONNECTED) {
                auto &writer = getPending(IOObject::OpWrite);
                while (!writer.empty()) {
                    auto p = static_cast<WriterCompletionToken*>(&writer.front());
                    std::error_code ec;
                    if (p->doWrite(ec)) {
                        p->notifyDone();
                        if (ec) {
                            cleanup(CancelReason::IOObjectShutdown);
                            return;
                        }
                        if (shutdown_flags_ & SHUT_WRITE_PENDING) {
                            shutdownWriteNow();
                        }
                    } else {
                        break;
                    }
                }
                if (writer.empty())
                    wio_.stop();
            }
        }
    }

    void closeRead() {
        rio_.stop();
        shutdown_flags_ |= SHUT_READ;
    }

    void forceClose() {
        wio_.stop();
        rio_.stop();
        socket_.close();
        s_ = CLOSED;
        shutdown_flags_ |= (SHUT_READ | SHUT_WRITE);
    }

    void failAllWrites() {
        auto &writer = getPending(IOObject::OpWrite);
        while (!writer.empty()) {
            auto p = static_cast<WriterCompletionToken*>(&writer.front());
            p->ec = std::make_error_code(std::errc::connection_aborted);
            p->notifyDone();
        }
    }
};

class ConnectFuture : public FutureBase<ConnectFuture, folly::Unit> {
public:
    using Item = folly::Unit;

    ConnectFuture(SocketChannel::Ptr ptr, const folly::SocketAddress &addr)
        : ptr_(ptr), addr_(addr) {}

    Poll<Item> poll() override {
        if (!tok_)
            tok_ = ptr_->doConnect(addr_);
        return tok_->poll();
    }
private:
    SocketChannel::Ptr ptr_;
    folly::SocketAddress addr_;
    io::intrusive_ptr<SocketChannel::ConnectCompletionToken> tok_;
};

class SockWriteFuture : public FutureBase<SockWriteFuture, ssize_t> {
public:
    using Item = ssize_t;

    SockWriteFuture(SocketChannel::Ptr ptr, std::unique_ptr<folly::IOBuf> buf)
        : ptr_(ptr), buf_(std::move(buf)) {}

    Poll<Item> poll() override {
        if (!tok_)
            tok_ = ptr_->doWrite(std::move(buf_));
        return tok_->poll();
    }
private:
    SocketChannel::Ptr ptr_;
    std::unique_ptr<folly::IOBuf> buf_;
    io::intrusive_ptr<SocketChannel::WriterCompletionToken> tok_;
};

class SockReadStream : public StreamBase<SockReadStream, std::unique_ptr<folly::IOBuf>> {
public:
    using Item = std::unique_ptr<folly::IOBuf>;

    struct StreamCompletionToken : public SocketChannel::ReaderCompletionToken {
    public:
        StreamCompletionToken() {}

        Poll<Optional<Item>> pollStream() {
            switch (getState()) {
            case STARTED:
                if (!buf_ || buf_->empty()) {
                    park();
                    return Poll<Optional<Item>>(not_ready);
                } else {
                    return makePollReady(Optional<Item>(std::move(buf_)));
                }
            case DONE:
                if (buf_ && !buf_->empty()) {
                    return makePollReady(Optional<Item>(std::move(buf_)));
                }
                if (ec) {
                    return Poll<Optional<Item>>(IOError("recv", ec));
                } else {
                    return makePollReady(Optional<Item>());
                }
            case CANCELLED:
                return Poll<Optional<Item>>(FutureCancelledException());
            }
        }

        void prepareBuffer(void **buf, size_t *bufLen) override {
            if (!buf_) buf_ = folly::IOBuf::create(2048);
            if (buf_->tailroom() == 0)
                buf_->prev()->appendChain(folly::IOBuf::create(2048));
            auto last = buf_->prev();
            *buf = last->writableTail();
            *bufLen = last->tailroom();
        }

        void dataReady(ssize_t size) override {
            buf_->prev()->append(size);
            notify();
        }
    private:
        std::unique_ptr<folly::IOBuf> buf_;
    };

    SockReadStream(SocketChannel::Ptr ptr)
        : ptr_(ptr) {}

    Poll<Optional<Item>> poll() override {
        if (!tok_)
            tok_ = ptr_->doRead(folly::make_unique<StreamCompletionToken>());
        return static_cast<StreamCompletionToken*>(tok_.get())->pollStream();
    }
private:
    SocketChannel::Ptr ptr_;
    io::intrusive_ptr<SocketChannel::ReaderCompletionToken> tok_;
};



}
}