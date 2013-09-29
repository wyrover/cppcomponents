//          Copyright John R. Bandela 2013.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once
#ifndef INCLUDE_GUARD_CPPCOMPONENTS_CHANNEL_HPP_05_29_2013_
#define INCLUDE_GUARD_CPPCOMPONENTS_CHANNEL_HPP_05_29_2013_

#include "future.hpp"
#include "implementation/queue.hpp"

namespace cppcomponents{

	typedef cppcomponents::uuid<0x4d327f1c, 0x7515, 0x4dc5, 0xb1e4, 0x4ff5051d2beb> channel_base_uuid_t;
	template<class T>
	struct IChannel : define_interface<combine_uuid<channel_base_uuid_t, typename uuid_of<T>::uuid_type>>{
		typedef delegate<void()> ClosedDelegate;

		use<IFuture<void>> Write(T);
		use<IFuture<void>> WriteError(cppcomponents::error_code);
		use<IFuture<T>> Read();
		void Close();
		void SetOnClosedRaw(use<ClosedDelegate>);
		void Complete();
		bool IsComplete();

		CPPCOMPONENTS_CONSTRUCT_TEMPLATE(IChannel, Write, WriteError, Read, Close,SetOnClosedRaw, Complete, IsComplete);

		CPPCOMPONENTS_INTERFACE_EXTRAS(IChannel){
			template<class F>
			void SetOnClosed(F f){
				this->get_interface().SetOnClosedRaw(make_delegate<ClosedDelegate>(f));
			}
		};
	};


	inline std::string ChannelDummyId(){ return "cppcomponents::uuid<0x158d0d7c, 0xcaf2, 0x4301, 0xa192, 0x0e2509204e93>"; }
	template<class T>
	using Channel_t = runtime_class < ChannelDummyId, object_interfaces < IChannel < T >> >;

	template<class T>
	using Channel = use<IChannel<T>>;

	template<class T>
	struct implement_channel : implement_runtime_class <
		implement_channel<T>,Channel_t<T>>
	{
		typedef IPromise<T> ipromise_t;
		typedef IPromise<use<ipromise_t>> ipromise_void_t;
		low_lock_queue<use<ipromise_t>> reader_promise_queue_;
		low_lock_queue<use<ipromise_void_t>> writer_promise_queue_;

		std::atomic<unsigned int> read_write_count_;
		std::atomic<bool> closed_;
		std::atomic<bool> complete_;

		use<delegate<void()>> on_closed_;

		implement_channel()
			: read_write_count_{ 0 }, closed_{ false }, complete_{false}
		{}

		struct read_write_counter{
			implement_channel* imp_;
			read_write_counter(implement_channel* i) : imp_{ i }{
				imp_->read_write_count_.fetch_add(1);
				if (imp_->closed_.load()){
					imp_->read_write_count_.fetch_sub(1);
					throw cppcomponents::error_abort{};
				}
			}

			~read_write_counter(){
				imp_->read_write_count_.fetch_sub(1);
			}
		};

		void SetOnClosedRaw(use < delegate < void() >> d){
			on_closed_ = d;
		}

		use<IFuture<void>> Write(T t){

			read_write_counter counter{ this };
			if (closed_.load()) return make_error_future<void>(cppcomponents::error_abort::ec);
			if (complete_.load()) return make_error_future<void>(cppcomponents::error_abort::ec);

			use<ipromise_t> rp;
			if ((reader_promise_queue_.consume(rp))){
				assert(rp);
				rp.Set(t);
				return make_ready_future();
			}
			else{

				auto p = make_promise<Promise<T>>();
				writer_promise_queue_.produce(p);
				auto f = p.template QueryInterface<IFuture<Promise<T>>>();
				return f.Then([t](Future<Promise<T>> f){
					Promise<T> p;
					try{
						p = f.Get();
						p.Set(t);
					}
					catch (std::exception& e){
						auto ec = error_mapper::error_code_from_exception(e);
						if(p) p.SetError(ec);
					}
				});

			}
		}
		use<IFuture<void>> WriteError(cppcomponents::error_code e){
			read_write_counter counter{ this };
			if (closed_.load()) return make_error_future<void>(cppcomponents::error_abort::ec);
			if (complete_.load()) return make_error_future<void>(cppcomponents::error_abort::ec);

			use<ipromise_t> rp;
			if ((reader_promise_queue_.consume(rp))){
				assert(rp);
				rp.SetError(e);
				return make_ready_future();
			}
			else{

				auto p = make_promise<Promise<T>>();
				writer_promise_queue_.produce(p);
				auto f = p.template QueryInterface < IFuture < Promise<T >> >();
				return f.Then([e](Future < Promise < T >> f){
					auto p = f.Get();
						p.SetError(e);

				});

			}
		}

		use<IFuture<T>> Read(){
			read_write_counter counter{ this };
			if (closed_.load()) return make_error_future<T>(cppcomponents::error_abort::ec);

			use<ipromise_void_t> p;
			writer_promise_queue_.consume(p);

			// If channel is complete and there is no pending write
			// Then read will never succeed so throw error
			if (complete_.load() && !p) return make_error_future<T>(cppcomponents::error_abort::ec);
			

			auto pr = implement_future_promise<T>::create().template QueryInterface<ipromise_t>();

			if (p){
				p.Set(pr);
			}
			else{
				reader_promise_queue_.produce(pr);

			}

			auto f = pr. template QueryInterface<IFuture<T>>();
			return f;
		}



		void Complete(){
			// Complete only called once
			if (complete_.exchange(true))return;
			try{

				// Busy wait for all read/writes to complete
				while (read_write_count_.load() > 0);


				// Clear out all the reads, because there will be no more writes
				// therefore any reads in the queue will just fail
				use<ipromise_t> rp;
				while (reader_promise_queue_.consume(rp)){
					if (rp){
						rp.SetError(cppcomponents::error_abort::ec);
					}
				}
			}
			catch (std::exception&){

			}
			
		}

		bool IsComplete(){
			return complete_.load();

		}
		void Close(){
			// Close only called once
			if (closed_.exchange(true))return;
			try{

				// Busy wait for all read/writes to complete
				while (read_write_count_.load() > 0);
				// Clear out all the writes
				use<ipromise_void_t> p;
				while (writer_promise_queue_.consume(p)){
					if (p){
						p.SetError(cppcomponents::error_abort::ec);
					}
				}

				// Clear out all the reads
				use<ipromise_t> rp;
				while (reader_promise_queue_.consume(rp)){
					if (rp){
						rp.SetError(cppcomponents::error_abort::ec);
					}
				}
			}
			catch (std::exception&){

			}
			if (on_closed_){
				try{
					on_closed_();
				}
				catch (std::exception&){

				}
			}
			on_closed_ = nullptr;


		}
		~implement_channel(){
			Close();
		}

	};


	template<class T>
	struct unique_channel{
		Channel<T> chan_;
		unique_channel(use < IChannel < T >> chan) : chan_(chan)
		{}

		~unique_channel()
		{
			if (get_ref()){
				get_ref().Close();
			}
		}

		unique_channel(unique_channel && other) : chan_(std::move(other.get_ref()))
		{
			other.release();
		}

		unique_channel& operator=(unique_channel && other){
			get_ref() = std::move(other.get_ref());
			other.release();
			return *this;
		}

		void release(){
			get_ref() = nullptr;
		}

		Channel<T> get(){
			return get_ref();
		}

	private:
		Channel<T>& get_ref(){
			return chan_;
		}
		unique_channel(const unique_channel&);
		unique_channel& operator=(const unique_channel&) ;
	};

	template<class T>
	Channel<T> make_channel(){
		return implement_channel<T>::create().template QueryInterface<IChannel<T>>();
	}



}



#endif