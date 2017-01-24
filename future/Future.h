#ifndef BERT_FUTURE_H
#define BERT_FUTURE_H

#include <future>
#include <atomic>
#include "Helper.h"
#include "Try.h"
#include "util/TimeScheduler.h"

namespace ananas
{

namespace internal
{

enum class Progress
{
    None,
    Timeout,
    Done
};

using TimeoutCallback = std::function<void ()>;

template <typename T>
struct State
{
    static_assert(std::is_same<T, void>::value || std::is_copy_constructible<T>(), "must be copyable or void");
    static_assert(std::is_same<T, void>::value || std::is_move_constructible<T>(), "must be movable or void");

    State() :
        ft_(pm_.get_future()),
        progress_(Progress::None),
        retrieved_ {false}
    {
    }

    // TODO: Using std::future/promise to avoid dirty work, but it's not a good idea.
    std::promise<T> pm_;
    std::future<T> ft_;

    // Protect then_, it's ineffecient, but for now I don't want to write future from scrath
    std::mutex thenLock_;
    std::function<void (Try<T>&& )> then_;
    Progress progress_;

    std::function<void (TimeoutCallback&& )> onTimeout_;
    std::atomic<bool> retrieved_;

    bool IsRoot() const { return !onTimeout_; }
};

} // end namespace internal


template <typename T>
class Future;

template <typename T>
class Promise
{
public:
    Promise() :
        state_(std::make_shared<internal::State<T>>())
    {
    }

    // TODO: C++11 lambda doesn't support move capture
    // just for compile, copy Promise is undefined, do NOT do that!
    Promise(const Promise&) = default;
    Promise& operator = (const Promise&) = default;

    Promise(Promise&& pm) = default;
    Promise& operator= (Promise&& pm) = default;

    template <typename SHIT = T>
    typename std::enable_if<!std::is_void<SHIT>::value, void>::type
    SetValue(SHIT&& t)
    {
        std::unique_lock<std::mutex> guard(state_->thenLock_);
        bool isRoot = state_->IsRoot();
        if (isRoot && state_->progress_ != internal::Progress::None)
            return;

        state_->progress_ = internal::Progress::Done;

        state_->pm_.set_value(t);
        if (state_->then_)
            state_->then_(std::move(t));
    }


    template <typename SHIT = T>
    typename std::enable_if<!std::is_void<SHIT>::value, void>::type
    SetValue(const SHIT& t)
    {
        std::unique_lock<std::mutex> guard(state_->thenLock_);
        bool isRoot = state_->IsRoot();
        if (isRoot && state_->progress_ != internal::Progress::None)
            return;

        state_->progress_ = internal::Progress::Done;

        state_->pm_.set_value(t);
        if (state_->then_)
            state_->then_(t);
    }

    template <typename SHIT = T>
    typename std::enable_if<!std::is_void<SHIT>::value, void>::type
    SetValue(Try<SHIT>&& t)
    {
        std::unique_lock<std::mutex> guard(state_->thenLock_);
        bool isRoot = state_->IsRoot();
        if (isRoot && state_->progress_ != internal::Progress::None)
            return;

        state_->pm_.set_value(t);
        if (state_->then_)
            state_->then_(std::move(t));
    }

    template <typename SHIT = T>
    typename std::enable_if<!std::is_void<SHIT>::value, void>::type
    SetValue(const Try<SHIT>& t)
    {
        std::unique_lock<std::mutex> guard(state_->thenLock_);
        bool isRoot = state_->IsRoot();
        if (isRoot && state_->progress_ != internal::Progress::None)
            return;

        state_->progress_ = internal::Progress::Done;

        state_->pm_.set_value(t);
        if (state_->then_)
            state_->then_(t);
    }

    template <typename SHIT = T>
    typename std::enable_if<std::is_void<SHIT>::value, void>::type
    SetValue(Try<void>&& )
    {
        std::unique_lock<std::mutex> guard(state_->thenLock_);
        bool isRoot = state_->IsRoot();
        if (isRoot && state_->progress_ != internal::Progress::None)
            return;

        state_->progress_ = internal::Progress::Done;

        state_->pm_.set_value();
        if (state_->then_)
            state_->then_(Try<void>());
    }

    template <typename SHIT = T>
    typename std::enable_if<std::is_void<SHIT>::value, void>::type
    SetValue(const Try<void>& )
    {
        std::unique_lock<std::mutex> guard(state_->thenLock_);
        bool isRoot = state_->IsRoot();
        if (isRoot && state_->progress_ != internal::Progress::None)
            return;

        state_->progress_ = internal::Progress::Done;

        state_->pm_.set_value();
        if (state_->then_)
            state_->then_(Try<void>());
    }

    template <typename SHIT = T>
    typename std::enable_if<std::is_void<SHIT>::value, void>::type
    SetValue()
    {
        std::unique_lock<std::mutex> guard(state_->thenLock_);
        bool isRoot = state_->IsRoot();
        if (isRoot && state_->progress_ != internal::Progress::None)
            return;

        state_->progress_ = internal::Progress::Done;

        state_->pm_.set_value();
        if (state_->then_)
            state_->then_(Try<void>());
    }

    Future<T> GetFuture()
    {
        bool expect = false;
        if (!state_->retrieved_.compare_exchange_strong(expect, true))
        {
            struct FutureAlreadyRetrieved {};
            throw FutureAlreadyRetrieved();
        }

        return Future<T>(state_);
    }

private:
    std::shared_ptr<internal::State<T>> state_;
};


template <typename T>
class Future
{
public:
    using InnerType = T;

    Future()
    {
    }

    Future(const Future&) = delete;
    void operator = (const Future&) = delete;

    Future(Future&& fut) = default;
    Future& operator= (Future&& fut) = default;

    explicit
    Future(std::shared_ptr<internal::State<T>> state) :
        state_(std::move(state))
    {
    }

    bool IsReady() const
    {
        // can be called multi times
        auto res = this->WaitFor(std::chrono::seconds(0));
        return res == std::future_status::ready;
    }

    template <typename SHIT = T>
    typename std::enable_if<!std::is_void<SHIT>::value, T>::type
    GetValue()
    {
        // only once
        return state_->ft_.get();
    }

    template <typename SHIT = T>
    typename std::enable_if<std::is_void<SHIT>::value, Try<void> >::type
    GetValue()
    {
        state_->ft_.get();
        return Try<void>();
    }

    template<typename R, typename P>
    std::future_status
    WaitFor(const std::chrono::duration<R, P>& dur) const
    {
        return state_->ft_.wait_for(dur);
    }

    template<typename Clock, typename Duration>
    std::future_status
    WaitUntil(const std::chrono::time_point<Clock, Duration>& abs) const
    {
        return state_->ft_.wait_until(abs);
    }

    template <typename F,
              typename R = internal::CallableResult<F, T> > 
    auto Then(F&& f) -> typename R::ReturnFutureType 
    {
        typedef typename R::Arg Arguments;
        return _ThenImpl<F, R>(std::forward<F>(f), Arguments());  
    }

    // modified from folly
    //1. F does not return future type
    template <typename F, typename R, typename... Args>
    typename std::enable_if<!R::IsReturnsFuture::value, typename R::ReturnFutureType>::type
    _ThenImpl(F&& f, internal::ResultOfWrapper<F, Args...> )
    {
        static_assert(std::is_void<T>::value ? sizeof...(Args) == 0 : sizeof...(Args) == 1,
                      "Then callback must take 0/1 argument");

        using FReturnType = typename R::IsReturnsFuture::Inner;

        Promise<FReturnType> pm;
        auto nextFuture = pm.GetFuture();

        std::unique_lock<std::mutex> guard(state_->thenLock_);
        if (IsReady())
        {
            Try<T> t(GetValue());
            auto result = WrapWithTry(f, t.template Get<Args>()...);
            pm.SetValue(std::move(result));
        }
        else
        {
            // 1. set pm's timeout callback
            nextFuture.SetOnTimeout([weak_parent = std::weak_ptr<internal::State<T>>(this->state_)](internal::TimeoutCallback&& cb) {
                    auto parent = weak_parent.lock();
                    if (!parent)
                        return;

                    {
                        std::unique_lock<std::mutex> guard(parent->thenLock_);
                        if (parent->progress_ != internal::Progress::None)
                            return;
                    
                        parent->progress_ = internal::Progress::Timeout;
                    }

                    if (!parent->IsRoot())
                        parent->onTimeout_(std::move(cb)); // propogate to the root
                    else
                        cb();
                });

            // 2. set this future's then callback
            SetCallback([func = std::move((typename std::decay<F>::type)f), prom = std::move(pm)](Try<T>&& t) mutable {
                // run callback, T can be void, thanks to folly Try<>
                auto result = WrapWithTry(func, t.template Get<Args>()...);
                // set next future's result
                prom.SetValue(std::move(result));
            });
        }

        return std::move(nextFuture);
    }

    //2. F return another future type
    template <typename F, typename R, typename... Args>
    typename std::enable_if<R::IsReturnsFuture::value, typename R::ReturnFutureType>::type
    _ThenImpl(F&& f, internal::ResultOfWrapper<F, Args...>)
    {
        static_assert(sizeof...(Args) <= 1, "Then must take zero/one argument");

        using FReturnType = typename R::IsReturnsFuture::Inner;

        Promise<FReturnType> pm;
        auto nextFuture = pm.GetFuture();

        std::unique_lock<std::mutex> guard(state_->thenLock_);
        if (IsReady())
        {
            Try<T> t(GetValue());
            auto f2 = f(t.template Get<Args>()...);
            f2.SetCallback([p2 = std::move(pm)](Try<FReturnType>&& b) mutable {
                p2.SetValue(std::move(b));
            });  
        }
        else
        {
            // 1. set pm's timeout callback
            nextFuture.SetOnTimeout([weak_parent = std::weak_ptr<internal::State<T>>(this->state_)](internal::TimeoutCallback&& cb) {
                    auto parent = weak_parent.lock();
                    if (!parent)
                        return;

                    {
                        std::unique_lock<std::mutex> guard(parent->thenLock_);
                        if (parent->progress_ != internal::Progress::None)
                            return;
                    
                        parent->progress_ = internal::Progress::Timeout;
                    }

                    if (!parent->IsRoot())
                        parent->onTimeout_(std::move(cb)); // propogate to the root
                    else
                        cb();

                });

            // 2. set this future's then callback
            SetCallback([func = std::move((typename std::decay<F>::type)f), prom = std::move(pm)](Try<T>&& t) mutable {
                // because func return another future:f2, when f2 is done, nextFuture can be done
                auto f2 = func(t.template Get<Args>()...);
                f2.SetCallback([p2 = std::move(prom)](Try<FReturnType>&& b) mutable {
                    p2.SetValue(std::move(b));
                });
            }); 
        }

        return std::move(nextFuture);
    }

    void SetCallback(std::function<void (Try<T>&& )>&& func)
    {
        this->state_->then_ = std::move(func);
    }

    void SetOnTimeout(std::function<void (internal::TimeoutCallback&& )>&& func)
    {
        this->state_->onTimeout_ = std::move(func);
    }

    /*
     * When register callbacks and timeout for a future like this:
     *
     *      Future<int> f;
     *      f.Then(xx).Then(yy).OnTimeout(zz);
     *
     * There will be 3 future objects created except f, we call f as root future.
     * The zz callback is register on the last future, however, timeout and future satisfication
     * can happened almost in the same time, we should ensure that both xx and yy will be called
     * or zz will be called, but they can't happened both or neither. So we pass the cb
     * to the root future, if we find out that root future is indeed timeout, we call cb there.
     */
    void OnTimeout(std::chrono::milliseconds duration,
                   internal::TimeoutCallback f,
                   TimeScheduler* scheduler)
    {
        scheduler->ScheduleOnceAfter(duration, [state = this->state_, cb = std::move(f)]() mutable {
                {
                    std::unique_lock<std::mutex> guard(state->thenLock_);

                    if (state->progress_ != internal::Progress::None)
                        return;

                    state->progress_ = internal::Progress::Timeout;
                }

                // Process timeout
                if (!state->IsRoot())
                    state->onTimeout_(std::move(cb)); // propogate to the root future
                else
                    cb();
        });
    }

private:
    std::shared_ptr<internal::State<T>> state_;
};

// Make ready future
template <typename T2>
inline Future<T2> MakeReadyFuture(T2&& value)
{
    Promise<T2> pm;
    auto f(pm.GetFuture());
    pm.SetValue(std::forward<T2>(value));

    return f;
}

inline Future<void> MakeReadyFuture()
{
    Promise<void> pm;
    auto f(pm.GetFuture());
    pm.SetValue();

    return f;
}

// When All
template <typename... FT>
typename internal::CollectAllVariadicContext<typename std::decay<FT>::type::InnerType...>::FutureType
WhenAll(FT&&... futures)
{
    auto ctx = std::make_shared<internal::CollectAllVariadicContext<typename std::decay<FT>::type::InnerType...>>();

    internal::CollectVariadicHelper<internal::CollectAllVariadicContext>( 
            ctx, std::forward<typename std::decay<FT>::type>(futures)...);

    return ctx->pm.GetFuture();
}


template <class InputIterator>
Future<
    std::vector<
    Try<typename std::iterator_traits<InputIterator>::value_type::InnerType>>>
    WhenAll(InputIterator first, InputIterator last)
{
    using T = typename std::iterator_traits<InputIterator>::value_type::InnerType;
    if (first == last)
        return MakeReadyFuture(std::vector<Try<T>>());

    struct CollectAllContext
    {
        CollectAllContext(int n) : results(n) {}
        ~CollectAllContext()
        { 
            // I don't think this line should appear
            // pm.SetValue(std::move(results));
        }
             
        Promise<std::vector<Try<T>>> pm;
        std::vector<Try<T>> results;
        std::atomic<size_t> collected{0};
    };

    auto ctx = std::make_shared<CollectAllContext>(std::distance(first, last));
                
    for (size_t i = 0; first != last; ++first, ++i)
    {
        first->SetCallback([ctx, i](Try<T>&& t) {
                ctx->results[i] = std::move(t);
                if (ctx->results.size() - 1 ==
                    std::atomic_fetch_add (&ctx->collected, std::size_t(1))) {
                    ctx->pm.SetValue(std::move(ctx->results));
                }
            });
    }

    return ctx->pm.GetFuture();
}

// When Any
template <class InputIterator>
Future<
  std::pair<size_t,
           Try<typename std::iterator_traits<InputIterator>::value_type::InnerType>>>
WhenAny(InputIterator first, InputIterator last)
{
    using T = typename std::iterator_traits<InputIterator>::value_type::InnerType;

    if (first == last)
    {
        return MakeReadyFuture(std::make_pair(size_t(0), Try<T>(T())));
    }

    struct CollectAnyContext
    {
        CollectAnyContext() {};
        Promise<std::pair<size_t, Try<T>>> pm;
        std::atomic<bool> done{false};
    };

    auto ctx = std::make_shared<CollectAnyContext>();
    for (size_t i = 0; first != last; ++first, ++i)
    {
        first->SetCallback([ctx, i](Try<T>&& t) {
            if (!ctx->done.exchange(true)) {
                ctx->pm.SetValue(std::make_pair(i, std::move(t)));
            }
       });
    }
           
    return ctx->pm.GetFuture();
}


// When N 
template <class InputIterator>
Future<
    std::vector<
    std::pair<size_t, Try<typename std::iterator_traits<InputIterator>::value_type::InnerType>>
    >
    >
    WhenN(size_t N, InputIterator first, InputIterator last)
{
    using T = typename std::iterator_traits<InputIterator>::value_type::InnerType;

    size_t nFutures = std::distance(first, last);
    const size_t needCollect = std::min(nFutures, N);

    if (needCollect == 0)
    {
        return MakeReadyFuture(std::vector<std::pair<size_t, Try<T>>>());
    }

    struct CollectNContext
    {
        CollectNContext(size_t _needs) : needs(_needs) {}
        Promise<std::vector<std::pair<size_t, Try<T>>>> pm;
    
        std::mutex mutex;
        std::vector<std::pair<size_t, Try<T>>> results;
        const size_t needs;
        bool done {false};
    };

    auto ctx = std::make_shared<CollectNContext>(needCollect);
    for (size_t i = 0; first != last; ++first, ++i)
    {
        first->SetCallback([ctx, i](Try<T>&& t) {
                std::unique_lock<std::mutex> guard(ctx->mutex);
                if (ctx->done)
                    return;
                    
                ctx->results.push_back(std::make_pair(i, std::move(t)));
                if (ctx->needs == ctx->results.size()) {
                    ctx->done = true;
                    ctx->pm.SetValue(std::move(ctx->results));
                }
            });
    }
           
    return ctx->pm.GetFuture();
}

} // end namespace ananas

#endif

