/**
 * @file   coopfuture.hpp
 * @author Trevor Hinkley
 * @date   01 July 2015
 * @brief  Header file for future/spawn/await functionality via Boost contexts.
 */

#ifndef COOPERATIVE_FUTURE_HPP
#define COOPERATIVE_FUTURE_HPP

#include <exception>
#include <deque>
#include <functional>
#include <boost/context/all.hpp>
#include <boost/coroutine/stack_context.hpp>
#include <boost/coroutine/stack_allocator.hpp>

namespace fut {

/// @cond HIDDEN_SYMBOLS

/// Helper structure for the Scheduler::spawn method.
template<typename T>
struct funcTraits { };

/// Helper structure for the Scheduler::spawn method.
template<typename R>
struct funcTraits<R(*)()> {
    typedef R resultType; ///< Return type signature.
};

/// Helper structure for the Scheduler::spawn method.
template<typename R>
struct funcTraits<R()> {
    typedef R resultType; ///< Return type signature.
};

/// Helper structure for the Scheduler::spawn method.
template<typename R, typename... A>
struct funcTraits<std::_Bind<R(*(A...))(A...)>> {
    typedef R resultType; ///< Return type signature.
};

/// @endcond


// Forward definition of the Future type
template <typename ValueType, typename ErrorType = std::exception> class Future;


/** @brief A structure to hold the linked list of waiting contexts.
 *
 *  Whenever a Future blocks, a context is set up and added to the list of
 *  waiting contexts. When the Future is fulfilled, this list is iterated
 *  through and the contexts are added to the tasks queue of the scheduler.
 *  This structure hold one element off the linked-list.
 */
typedef struct FContextCons{
	/// The context that will be released when the Future is fulfilled.
	boost::context::fcontext_t context;
	/// Next item in the list.
	struct FContextCons* nextPointer;
}FContextCons;


/** @brief The exception to throw if Future::setResult or Future::setError is
 *         called twice on the same Future.
 */
struct FutureAlreadyFulfilled : std::exception{
	/** The interface required by std::exception for textual representation
	 *  of the nature of the error.
	 *
	 *  @returns A string explaining that the Future was already resolved.
	 */
	const char* what() const noexcept {
		return "Result/Error already set. Future already fulfilled.\n";
	}
};

/** @brief A class to encapsulate IO loops and callbacks.
 *
 *  The class scheduler is used to create handles, which encapsulate the waiting
 *  for a specific event to occur. Whilst waiting, an IO method is called
 *  repeatedly by the loopHandler and, after each call, a queue is checked to
 *  see if any handles have cleared and the calling context resumed.
 */
class Scheduler {
public:
	/** @brief Constructs the object with callback and call-context size, using
	 *         a boost::coroutines stack-allocator for stack management.
	 *  @param cb Callback to use inside the resolution loop.
	 *  @param stackAlloc A stack-allocator structure from boost::coroutine.
	 *  @param stackSize The size (in bytes) of the stack for each context to
	 *         be used for entry into the resolution loop.
	 */
	template<typename Allocator = boost::coroutines::stack_allocator>
	Scheduler(std::function<void()> cb,
	          Allocator stackAlloc = boost::coroutines::stack_allocator(),
	          std::size_t stackSize = Allocator::traits_type::default_size()) :
		innerCallback(cb),
		contextSize(stackSize),
		stackAllocate(std::bind(&Allocator::allocate, &stackAlloc,
		                        std::placeholders::_1, std::placeholders::_2)),
		stackDeallocate(std::bind(&Allocator::deallocate, &stackAlloc,
		                          std::placeholders::_1)) {};
	
#ifdef BOOST_ASIO_IO_SERVICE_HPP
	/** @brief Constructs the object from a boost::asio::io_service and
	 *         call-context size, using a boost::coroutines stack-allocator for
	 *         stack management.
	 *  @param io Boost IO object, from which run_one will be used.
	 *  @param stackAlloc A stack-allocator structure from boost::coroutine.
	 *  @param stackSize The size (in bytes) of the stack for each context to
	 *         be used for entry into the resolution loop.
	 */
	template<typename Allocator = boost::coroutines::stack_allocator>
	Scheduler(boost::asio::io_service& io,
	          Allocator stackAlloc = boost::coroutines::stack_allocator(),
	          std::size_t stackSize = Allocator::traits_type::default_size()) :
		innerCallback(std::bind((std::size_t(boost::asio::io_service::*)())
		                                  &boost::asio::io_service::run_one,
		                        &io)),
		contextSize(stackSize),
		stackAllocate(std::bind(&Allocator::allocate, &stackAlloc,
		                        std::placeholders::_1, std::placeholders::_2)),
		stackDeallocate(std::bind(&Allocator::deallocate, &stackAlloc,
		                          std::placeholders::_1)) {};
#endif
	
	
	/** @brief This method runs the inner loop until the context is ready.
	 *  @param context The context to wait for completion upon.
	 *  @param preserveFPRegisters Whether to save the floating point regs in
	 *         the context's stack.
	 */
	void waitUntilReady(boost::context::fcontext_t* context,
	                    bool preserveFPRegisters = false);
	
	/** @brief This method notifies that the waiting context can be resumed.
	 *  @param cons The context list to resume. Each member should have been
	 *         passed to Schedule::waitUntilReady.
	 */
	void notifyReady(FContextCons* cons) noexcept;
	
	/** @brief Spawn a new asynchronous method.
	 *  @param call The function which should take no arguments but which can
	 *         return a value. Intended to be constructed with std::bind.
	 *  @returns an appropriate Future object for the return type.
	 */
	template <typename E = std::exception,
	          typename Func,
	          typename Traits = funcTraits<Func>,
	          typename R = typename Traits::resultType >
	typename std::enable_if<!std::is_void<R>::value,
	                         std::unique_ptr<Future<R, E>>
	  >::type spawn(Func call) noexcept;
	
	/** @brief Spawn a new asynchronous method with no return value.
	 *  @param call The function which should take no arguments and does not
	 *         return a value. Intended to be constructed with std::bind.
	 *  @returns An appropriate Future object with void return type.
	 */
	template <typename E = std::exception,
	          typename Func,
	          typename Traits = funcTraits<Func>,
	          typename R = typename Traits::resultType >
	typename std::enable_if<std::is_void<R>::value,
	                        std::unique_ptr<Future<void, E>>
	  >::type spawn(Func call) noexcept;

private:
	/** @brief Inner loop for context resolution.
	*/
	void loop() noexcept;
	
	/** @brief This method is used as a context switch trampoline to enter the
	 *         the resolution loop.
	 *  @param arg The argument provided by jump_fcontext, which should contain
	 *             the *this* pointer.
	 */
	static void loopEntry(intptr_t arg) noexcept {
		reinterpret_cast<Scheduler*>(arg)->loop();
	}
	
	/// Queue to hold the completed/ready contexts for returning from the loop.
	std::deque<boost::context::fcontext_t> ready;
	
	/// Size (in bytes) of the stack allocated for the resolution contexts.
	std::size_t contextSize;
	
	/// Callback function for use in the resolution loop.
	std::function<void()> innerCallback;
	
	/// Allocator function, which conforms to the stack-allocator concept.
	std::function<void(boost::coroutines::stack_context&, std::size_t)>
	  stackAllocate;
	
	/// Deallocator function, which conforms to the stack-allocator concept.
	std::function<void(boost::coroutines::stack_context&)> stackDeallocate;
	
	/// Queue to hold asynchronous (spawned) tasks.
	std::deque<std::function<void()>> tasks;
};

/** Goes through the linked list provided and adds each individual
 *  *boost::context::fcontext_t* contained therein to the queue of completed
 *  contexts for processing by the inner loop.
 */
void Scheduler::notifyReady(FContextCons* cons) noexcept {
	while (cons != nullptr) {
		ready.push_back(cons->context);
		cons = cons->nextPointer;
	}
}

/** Allocates a new *boost::coroutines::stack_context* object and calls the
 *  loopEntry function within a new context. When the context returns, the
 *  *stack_context* is deallocated.
 *  TODO: Some way of selecting whether the floating point registers are
 *        preserved in the context.
 */
void Scheduler::waitUntilReady(boost::context::fcontext_t* context,
                               bool preserveFPRegisters) {
	boost::coroutines::stack_context sc;
	stackAllocate(sc, contextSize);
	
	boost::context::jump_fcontext(
	   context,
	   boost::context::make_fcontext(
	      sc.sp,
	      sc.size,
	      loopEntry),
	   (intptr_t)this,
	   preserveFPRegisters);
	
	stackDeallocate(sc);
}

/** This method will continuously poll the Scheduler::ready queue until a
 *  completed context is encountered. After each attempt at retreiveal, an
 *  iteration of the supplied fulfilment function is applied.
 */
void Scheduler::loop() noexcept {
	boost::context::fcontext_t junkContext;
	boost::context::fcontext_t exitContext;
	
	try {
		while (ready.size() == 0)
			if (tasks.size() > 0) {
				auto task = tasks.front();
				tasks.pop_front();
				task();
			} else
				innerCallback();
	} catch (FutureAlreadyFulfilled) {
		std::cerr << "A Future was fulfilled a second time within a "
		             "Scheduler loop. This is non-recoverable.\n";
		std::terminate();
	}
	exitContext = ready.front();
	ready.pop_front();
	boost::context::jump_fcontext(&junkContext, exitContext, 0);
}

/** Creates a new Future (on the heap) and a lambda expression, wrapping the
 *  provided function object. This is then pushed to the scheduler tasks queue
 *  for completion during an IO loop run.
 */
template <typename E,
          typename Func,
          typename Traits,
          typename R>
typename std::enable_if<!std::is_void<R>::value, std::unique_ptr<Future<R, E>>
  >::type Scheduler::spawn(Func call) noexcept {
	auto fut = new Future<R, E>(*this);
	tasks.push_back([=] {
		try {
			fut->setResult(call());
		} catch (FutureAlreadyFulfilled faf) {
			throw faf;
		} catch (E e) {
			fut->setError(e);
		} catch (...) {
			fut->setUnexpected();
		}
	});
	return std::unique_ptr<Future<R, E>>(fut);
}

/** Creates a new void Future (on the heap) and a lambda expression, wrapping
 *  the provided function object. This is then pushed to the scheduler tasks
 *  queue for completion during an IO loop run.
 */
template <typename E,
          typename Func,
          typename Traits,
          typename R>
typename std::enable_if<std::is_void<R>::value,std::unique_ptr<Future<void, E>>
  >::type Scheduler::spawn(Func call) noexcept {
	auto fut = new Future<void, E>(*this);
	tasks.push_back([=] {
		try {
			call();
			fut->setResult();
		} catch (FutureAlreadyFulfilled faf) {
			throw faf;
		} catch (E e) {
			fut->setError(e);
		} catch (...) {
			fut->setUnexpected();
		}
	});
	return std::unique_ptr<Future<void, E>>(fut);
}

/** @brief Base class for the specialization and general Future objects.
 *
 *  The Future class is used to specify that a result will be forthcoming at
 *  some point in the future. It is therefore a building block for asynchronous
 *  programing. A function can return a Future rather than an actual result and
 *  then set up a mechanism whereby the result is provided. When a Future is
 *  cast into its underlying value, if the value has not currently been provided
 *  then the scheduler is invoked in an attempt to fulfill the value.
 *
 *  This class is NOT thread-safe and is intended for cooperative multi-tasking.
 */
template <typename ValueType, typename ErrorType>
class FutureBase {
public:
	/** @brief A Future is constructed by providing a scheduler on which to
	 *         resolve the value, if it is not provided before it is needed.
	 *  @param sched A reference to an initialized Scheduler.
	 */
	FutureBase(Scheduler& sched) :
		scheduler(sched), state(State::UNRESOLVED) {};
	
	/** @brief Fulfills an unsuccesful Future with the exception to throw.
	 *  @param newError the exception to throw on waiting code.
	 */
	void setError(ErrorType) throw (FutureAlreadyFulfilled);
	
	/** @brief Future was fulfilled with an exception outside of its scope.
	 */
	void setUnexpected() throw (FutureAlreadyFulfilled);
	
protected:
	/* @brief Used by various submethods to indicated the Future is fulfilled.
	*/
	void complete() throw (FutureAlreadyFulfilled);
	
	/** @enum State 
	 *  A strongly typed enum class representing the state of a Future.
	 */
	enum class State {
		UNRESOLVED, ///< Starting state for a Future, no value has yet been set.
		SUCCESS,    ///< Future has a valid return value for retrieval.
		FAILURE,    ///< Future will raise an exception on retrieval.
		UNEXPECTED  ///< Future caught an unexpected error.
	};
	
	/// The scheduler to invoke if blocking is necessary.
	Scheduler& scheduler;
	
	/// The current state, determines which view of the union member is valid.
	State state;
	
	/** @brief The contents of a Future object.
	 *
	 *  This can be any ONE of the below values and is hence represented by a
	 *  union. The union is deliminated by the enum State in the Future class
	 *  and the destructor of that class is responsible for correct destruction
	 *  of the union.
	 */
	union Contents {
		/// The context for a waiting Future, valid whilst in State::UNRESOLVED.
		FContextCons* cons;
		/// The exception to raise for a failed Future, valid in State::FAILURE.
		ErrorType error;
		/// The resulting value for a succesful Future, valid in State::SUCCESS.
		ValueType result;
		/// The union starts with a nullptr in the cons list
		Contents() : cons(nullptr) {};
		/// Blank destructor: The Future destructor is responsible for clean-up.
		~Contents() {};
	} contents; ///< Singleton instantiation of Contents.
};

/** Store the reason for failure in the Future. If there is a context
 *  list blocking on this Future then release those contexts for the scheduler
 *  to process.
 */
template <typename ValueType, typename ErrorType>
void FutureBase<ValueType, ErrorType>::setError(ErrorType newError)
  throw (FutureAlreadyFulfilled) {
	complete();
	state = State::FAILURE;
	contents.error = newError;
}

/** Store the unexpected failure in the Future. If there is a context
 *  list blocking on this Future then release those contexts for the scheduler
 *  to process.
 */
template <typename ValueType, typename ErrorType>
void FutureBase<ValueType, ErrorType>::setUnexpected()
  throw (FutureAlreadyFulfilled) {
	complete();
	state = State::UNEXPECTED;
}

/** Check whether the Future was already fulfilled and release any waiting
 *  contexts.
 */
template <typename ValueType, typename ErrorType>
void FutureBase<ValueType, ErrorType>::complete() throw (FutureAlreadyFulfilled) {
	if (state != State::UNRESOLVED)
		throw new FutureAlreadyFulfilled;
	
	if (contents.cons != nullptr) 
		scheduler.notifyReady(contents.cons);
}

/** @brief A class to encapsulate the promise of a future result.
 *
 *  This sub-class of FutureBase handles all those cases where ValueType is
 *  not void.
 */
template <typename ValueType, typename ErrorType>
class Future : public FutureBase<ValueType, ErrorType> {
public:
	using FutureBase<ValueType, ErrorType>::FutureBase;
	
	/** @brief Fulfills a succesful Future with a resulting value.
	 *  @param newResult The value to provide to waiting code.
	 */
	void setResult(ValueType newResult) throw (FutureAlreadyFulfilled);
	
	/** @brief   The public interface to retrieve the value is to cast the
	 *           Future into the underlying type.
	 *  @returns The underlying value or throws the exception.
	 */
	inline operator ValueType() throw(ErrorType) { return await(); }
	
	/** @brief Class destructor
	 */
	~Future();
	
	/** @brief Returns the underyling result/exception, blocking if necessary.
	 */
	ValueType await() throw(ErrorType);

protected:
	typedef typename FutureBase<ValueType, ErrorType>::State State;
	using FutureBase<ValueType, ErrorType>::state;
	using FutureBase<ValueType, ErrorType>::contents;
	using FutureBase<ValueType, ErrorType>::scheduler;
	using FutureBase<ValueType, ErrorType>::complete;
};

/*  If the outcome of the Future is set then destroy the result or
 *  the exception as appropriate.
 */
template <typename ValueType, typename ErrorType>
Future<ValueType, ErrorType>::~Future() {
	switch(this->state) {
	case State::SUCCESS:
		contents.result.~ValueType();
		break;
	case State::FAILURE:
		contents.error.~ErrorType();
		break;
	}
}


/** If the Future has completed then throw the error or return the result
 *  as appropriate, otherwise, add a waiting context node to the cons list
 *  and jump to the scheduler (block until Future is resolved).
 */
template <typename ValueType, typename ErrorType>
ValueType Future<ValueType, ErrorType>::await() throw(ErrorType) {
	switch (state) {
	// Future succeeded - return the fulfilment result
	case State::SUCCESS:
		return contents.result;
	
	// Failed Future will throw the stored error on sync
	case State::FAILURE:
		throw contents.error;
	
	// Unexpected Future will call the unexpected handler
	case State::UNEXPECTED:
		std::unexpected();
		//this prior statement does not return.
	
	// Unresolved Future must block until the scheduler fulfills it.
	case State::UNRESOLVED:
		FContextCons cons;
		cons.nextPointer = contents.cons;
		contents.cons = &cons;
		scheduler.waitUntilReady(&cons.context);
		return await();
	}
}

/** Store the result of the operation in the Future. If there is a context
 *  list blocking on this Future then release those contexts for the scheduler
  *  to process.
 */
template <typename ValueType, typename ErrorType>
void Future<ValueType, ErrorType>::setResult(ValueType newResult)
  throw (FutureAlreadyFulfilled) {
	complete();
	state = State::SUCCESS;
	contents.result = newResult;
}


/** @brief A class to encapsulate a resultless asynchronous task.
 *
 *  This is a specialization of the Future class. It is used as a form of
 *  barrier, where the Future will not have a result set on it but can throw
 *  an error and can be blocked on.
 */
template <typename ErrorType>
class Future<void, ErrorType> : public FutureBase<void*, ErrorType> {
public:
	using FutureBase<void*, ErrorType>::FutureBase;
	
	/** @brief Fulfills a succesful Future.
	 */
	void setResult() throw (FutureAlreadyFulfilled);
	
	/** @brief Class destructor
	 */
	~Future();
	
	/** @brief Returns the underyling result/exception, blocking if necessary.
	 */
	void await() throw(ErrorType);

protected:
	typedef typename FutureBase<void*, ErrorType>::State State;
	using FutureBase<void*, ErrorType>::state;
	using FutureBase<void*, ErrorType>::contents;
	using FutureBase<void*, ErrorType>::scheduler;
	using FutureBase<void*, ErrorType>::complete;
};

/** If the outcome of the Future is set then destroy the result or
 *  the exception as appropriate.
 */
template <typename ErrorType>
Future<void, ErrorType>::~Future() {
	if (state == State::FAILURE) 
		contents.error.~ErrorType();
}

/** If the Future has completed then throw the error or return silently
 *  as appropriate, otherwise, add a waiting context node to the cons list
 *  and jump to the scheduler (block until Future is resolved).
 */
template <typename ErrorType>
void Future<void, ErrorType>::await() throw(ErrorType) {
	switch (state) {
	// Void Future - no return
	case State::SUCCESS:
		return;
	
	// Void Future can still throw an error
	case State::FAILURE:
		throw contents.error;
	
	// Unexpected Future will call the unexpected handler
	case State::UNEXPECTED:
		std::unexpected();
		//this prior statement does not return.
	
	// Unresolved Future must block until the scheduler fulfills it.
	case State::UNRESOLVED:
		FContextCons cons;
		cons.nextPointer = contents.cons;
		contents.cons = &cons;
		scheduler.waitUntilReady(&cons.context);
		await();
	}
}

/** Store the success of the operation in the void Future. If there is a context
 *  list blocking on this Future then release those contexts for the scheduler
 *  to process.
 */
template <typename ErrorType>
void Future<void, ErrorType>::setResult() throw (FutureAlreadyFulfilled) {
	complete();
	state = State::SUCCESS;
}

} // End of namespace fut

#endif
