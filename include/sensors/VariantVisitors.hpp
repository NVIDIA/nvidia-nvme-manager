/*
 * SPDX-FileCopyrightText: Copyright (c) 2022-2024 NVIDIA CORPORATION &
 * AFFILIATES. SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <boost/type_index.hpp>

#include <concepts>
#include <stdexcept>
#include <string>
#include <vector>

namespace details
{
template <typename U>
struct VariantToNumericVisitor
{
    template <typename T>
    U operator()(const T& t) const
    {
        if constexpr (std::is_arithmetic_v<T>)
        {
            return static_cast<U>(t);
        }
        throw std::invalid_argument(
            "Cannot translate type " +
            boost::typeindex::type_id<T>().pretty_name() + " to " +
            boost::typeindex::type_id<U>().pretty_name());
    }
};
} // namespace details

using VariantToFloatVisitor = details::VariantToNumericVisitor<float>;
using VariantToIntVisitor = details::VariantToNumericVisitor<int>;
using VariantToUnsignedIntVisitor =
    details::VariantToNumericVisitor<unsigned int>;
using VariantToDoubleVisitor = details::VariantToNumericVisitor<double>;

struct VariantToStringVisitor
{
    template <typename T>
    std::string operator()(const T& t) const
    {
        if constexpr (std::is_same_v<T, std::string>)
        {
            return t;
        }
        else if constexpr (std::is_arithmetic_v<T>)
        {
            return std::to_string(t);
        }
        throw std::invalid_argument(
            "Cannot translate type " +
            boost::typeindex::type_id<T>().pretty_name() + " to string");
    }
};
