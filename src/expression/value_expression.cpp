//
// Created by JinHai on 2022/8/5.
//

#include "value_expression.h"

#include <utility>

namespace infinity {

ValueExpression::ValueExpression(LogicalType data_type, std::any value)
    : BaseExpression(ExpressionType::kValue, {}), data_type_(data_type), value_(std::move(value)) {}

ValueExpression::ValueExpression(LogicalType data_type)
    : BaseExpression(ExpressionType::kValue, {}), data_type_(data_type), value_(std::any()) {}

std::string
ValueExpression::ToString() const {
    return std::string();
}

void
ValueExpression::AppendToChunk(Chunk& chunk) {
    chunk.Append(value_);
}

}
