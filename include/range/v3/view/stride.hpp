// Range v3 library
//
//  Copyright Eric Niebler 2013-2014
//
//  Use, modification and distribution is subject to the
//  Boost Software License, Version 1.0. (See accompanying
//  file LICENSE_1_0.txt or copy at
//  http://www.boost.org/LICENSE_1_0.txt)
//
// Project home: https://github.com/ericniebler/range-v3
//

#ifndef RANGES_V3_VIEW_STRIDE_HPP
#define RANGES_V3_VIEW_STRIDE_HPP

#include <atomic>
#include <utility>
#include <type_traits>
#include <range/v3/range_fwd.hpp>
#include <range/v3/size.hpp>
#include <range/v3/distance.hpp>
#include <range/v3/begin_end.hpp>
#include <range/v3/range_traits.hpp>
#include <range/v3/range_concepts.hpp>
#include <range/v3/range_adaptor.hpp>
#include <range/v3/utility/box.hpp>
#include <range/v3/utility/bindable.hpp>
#include <range/v3/utility/iterator.hpp>

namespace ranges
{
    inline namespace v3
    {
        template<typename Rng>
        struct strided_view
          : range_adaptor<strided_view<Rng>, Rng>
        {
        private:
            friend range_access;
            using size_type_ = range_size_t<Rng>;
            using difference_type_ = range_difference_t<Rng>;

            // Bidirectional stride iterators need a runtime boolean to keep track
            // of when the offset variable is dirty and needs to be lazily calculated.
            // Ditto for random-access stride iterators when the end is a sentinel.
            // If the size of the range is known a priori, then the runtime boolean
            // is always unnecessary.
            using dirty_t =
                detail::conditional_t<
                    (BidirectionalIterable<Rng>() && !SizedIterable<Rng>()),
                    mutable_<std::atomic<bool>>,
                    constant<bool, false>>;

            // Bidirectional and random-access stride iterators need to remember how
            // far past they end they are, so that when they're decremented, they can
            // visit the correct elements.
            using offset_t =
                detail::conditional_t<
                    (BidirectionalIterable<Rng>()),
                    mutable_<std::atomic<difference_type_>>,
                    constant<difference_type_, 0>>;

            difference_type_ stride_;

            struct adaptor : iterator_adaptor_base, private dirty_t, private offset_t
            {
            private:
                using iterator = ranges::range_iterator_t<Rng>;
                using adaptor_pair = std::pair<iterator, adaptor>;
                strided_view const *rng_;
                dirty_t & dirty() { return *this; }
                dirty_t const & dirty() const { return *this; }
                offset_t & offset() { return *this; }
                offset_t const & offset() const { return *this; }
                void clean() const
                {
                    // Harmless race here. Two threads might compute offset and set it
                    // independently, but the result would be the same.
                    if(dirty())
                    {
                        do_clean();
                        dirty() = false;
                    }
                }
                void do_clean() const
                {
                    auto tmp = ranges::distance(rng_->base()) % rng_->stride_;
                    offset() = 0 != tmp ? rng_->stride_ - tmp : tmp;
                }
            public:
                adaptor() = default;
                adaptor(strided_view const &rng, begin_tag)
                  : dirty_t(false), offset_t(0), rng_(&rng)
                {}
                adaptor(strided_view const &rng, end_tag)
                  : dirty_t(true), offset_t(0), rng_(&rng)
                {
                    // Opportunistic eager cleaning when we can do so in O(1)
                    if(BidirectionalIterable<Rng>() && SizedIterable<Rng>())
                        do_clean();
                }
                void next(iterator &it)
                {
                    RANGES_ASSERT(0 == offset());
                    RANGES_ASSERT(it != ranges::end(rng_->base()));
                    offset() = advance_bounded(it, rng_->stride_ + offset(),
                        ranges::end(rng_->base()));
                }
                CONCEPT_REQUIRES(BidirectionalIterable<Rng>())
                void prev(iterator &it)
                {
                    clean();
                    offset() = advance_bounded(it, -rng_->stride_ + offset(),
                        ranges::begin(rng_->base()));
                    RANGES_ASSERT(0 == offset());
                }
                CONCEPT_REQUIRES(RandomAccessIterable<Rng>())
                difference_type_ distance_to(iterator here, adaptor_pair there) const
                {
                    clean();
                    there.second.clean();
                    RANGES_ASSERT(rng_ == there.second.rng_);
                    RANGES_ASSERT(0 == ((there.first - here) +
                        (there.second.offset() - offset())) % rng_->stride_);
                    return ((there.first - here) +
                        (there.second.offset() - offset())) / rng_->stride_;
                }
                CONCEPT_REQUIRES(RandomAccessIterable<Rng>())
                void advance(iterator &it, difference_type_ n)
                {
                    if(n != 0)
                        clean();
                    if(0 < n)
                        offset() = advance_bounded(it, n * rng_->stride_ + offset(),
                            ranges::end(rng_->base()));
                    else if(0 > n)
                        offset() = advance_bounded(it, n * rng_->stride_ + offset(),
                            ranges::begin(rng_->base()));
                }
            };
            // If the underlying sequence object doesn't model BoundedIterable, then we can't
            // decrement the end and there's no reason to adapt the sentinel. Strictly
            // speaking, we don't have to adapt the end iterator of Input and Forward
            // Iterables, but in the interests of making the resulting stride view model
            // BoundedRange, adapt it anyway.
            auto end_adaptor_(concepts::Iterable*) const -> sentinel_adaptor_base
            {
                return {};
            }
            auto end_adaptor_(concepts::BoundedIterable*) const -> adaptor
            {
                return {*this, end_tag{}};
            }

            adaptor begin_adaptor() const
            {
                return {*this, begin_tag{}};
            }
            detail::conditional_t<(BoundedIterable<Rng>()), adaptor, sentinel_adaptor_base>
            end_adaptor() const
            {
                return strided_view::end_adaptor_(bounded_iterable_concept<Rng>());
            }
        public:
            strided_view() = default;
            strided_view(Rng &&rng, difference_type_ stride)
              : range_adaptor_t<strided_view>{std::forward<Rng>(rng)}
              , stride_(stride)
            {
                RANGES_ASSERT(0 < stride_);
            }
            CONCEPT_REQUIRES(SizedIterable<Rng>())
            size_type_ size() const
            {
                return (ranges::size(this->base()) + static_cast<size_type_>(stride_) - 1) /
                    static_cast<size_type_>(stride_);
            }
        };

        namespace view
        {
            struct stride_fn : bindable<stride_fn>
            {
                template<typename Rng>
                static strided_view<Rng>
                invoke(stride_fn, Rng && rng, range_difference_t<Rng> step)
                {
                    CONCEPT_ASSERT(InputIterable<Rng>());
                    return {std::forward<Rng>(rng), step};
                }

                template<typename Difference>
                static auto invoke(stride_fn stride, Difference step) ->
                    decltype(stride.move_bind(std::placeholders::_1, std::move(step)))
                {
                    CONCEPT_ASSERT(Integral<Difference>());
                    return stride.move_bind(std::placeholders::_1, std::move(step));
                }
            };

            RANGES_CONSTEXPR stride_fn stride{};
        }
    }
}

#endif
