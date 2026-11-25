#include "zl/parser/ast.hpp"

namespace zl {

void forEachChild(const AstNode* node, const std::function<void(const AstNode*)>& visit) {
    if (!node) return;

    // Statements and expressions are enumerated here; declaration-level nodes
    // are deliberately absent (see the note in ast.hpp).
    switch (node->kind) {
        case NodeKind::BlockStmt: {
            const auto& block = static_cast<const BlockStmt&>(*node);
            for (const auto& child : block.statements) visit(child.get());
            return;
        }
        case NodeKind::VarDecl: {
            visit(static_cast<const VarDecl&>(*node).initializer.get());
            return;
        }
        case NodeKind::LogStmt: {
            visit(static_cast<const LogStmt&>(*node).argument.get());
            return;
        }
        case NodeKind::LogExpr: {
            visit(static_cast<const LogExpr&>(*node).argument.get());
            return;
        }
        case NodeKind::IfStmt: {
            const auto& statement = static_cast<const IfStmt&>(*node);
            for (const auto& branch : statement.branches) {
                visit(branch.condition.get());
                visit(branch.body.get());
            }
            visit(statement.elseBody.get());
            return;
        }
        case NodeKind::ReturnStmt: {
            visit(static_cast<const ReturnStmt&>(*node).value.get());
            return;
        }
        case NodeKind::ForStmt: {
            const auto& statement = static_cast<const ForStmt&>(*node);
            visit(statement.iterable.get());
            visit(statement.start.get());
            visit(statement.end.get());
            visit(statement.step.get());
            visit(statement.body.get());
            return;
        }
        case NodeKind::WhileStmt: {
            const auto& statement = static_cast<const WhileStmt&>(*node);
            visit(statement.condition.get());
            visit(statement.body.get());
            return;
        }
        case NodeKind::RepeatStmt: {
            const auto& statement = static_cast<const RepeatStmt&>(*node);
            visit(statement.body.get());
            visit(statement.condition.get());
            return;
        }
        case NodeKind::TryStmt: {
            const auto& statement = static_cast<const TryStmt&>(*node);
            visit(statement.tryBlock.get());
            for (const auto& clause : statement.catches) visit(clause.block.get());
            visit(statement.finallyBlock.get());
            return;
        }
        case NodeKind::ThrowStmt: {
            visit(static_cast<const ThrowStmt&>(*node).value.get());
            return;
        }
        case NodeKind::ExprStmt: {
            visit(static_cast<const ExprStmt&>(*node).expression.get());
            return;
        }
        case NodeKind::UnaryExpr: {
            visit(static_cast<const UnaryExpr&>(*node).operand.get());
            return;
        }
        case NodeKind::AwaitExpr: {
            visit(static_cast<const AwaitExpr&>(*node).operand.get());
            return;
        }
        case NodeKind::BinaryExpr: {
            const auto& expression = static_cast<const BinaryExpr&>(*node);
            visit(expression.left.get());
            visit(expression.right.get());
            return;
        }
        case NodeKind::AssignExpr: {
            visit(static_cast<const AssignExpr&>(*node).value.get());
            return;
        }
        case NodeKind::CallExpr: {
            for (const auto& argument : static_cast<const CallExpr&>(*node).arguments) visit(argument.get());
            return;
        }
        case NodeKind::SuperCallExpr: {
            for (const auto& argument : static_cast<const SuperCallExpr&>(*node).arguments) visit(argument.get());
            return;
        }
        case NodeKind::SuperMethodCallExpr: {
            for (const auto& argument : static_cast<const SuperMethodCallExpr&>(*node).arguments) visit(argument.get());
            return;
        }
        case NodeKind::MethodCallExpr: {
            const auto& expression = static_cast<const MethodCallExpr&>(*node);
            visit(expression.object.get());
            for (const auto& argument : expression.arguments) visit(argument.get());
            return;
        }
        case NodeKind::CollectionLiteral: {
            const auto& literal = static_cast<const CollectionLiteral&>(*node);
            for (const auto& element : literal.elements) visit(element.get());
            for (const auto& entry : literal.entries) {
                visit(entry.first.get());
                visit(entry.second.get());
            }
            return;
        }
        case NodeKind::NewExpr: {
            for (const auto& argument : static_cast<const NewExpr&>(*node).arguments) visit(argument.get());
            return;
        }
        case NodeKind::FieldAccessExpr: {
            visit(static_cast<const FieldAccessExpr&>(*node).object.get());
            return;
        }
        case NodeKind::IndexAccessExpr: {
            const auto& expression = static_cast<const IndexAccessExpr&>(*node);
            visit(expression.object.get());
            visit(expression.index.get());
            return;
        }
        case NodeKind::FieldAssignExpr: {
            const auto& expression = static_cast<const FieldAssignExpr&>(*node);
            visit(expression.object.get());
            visit(expression.value.get());
            return;
        }
        case NodeKind::DataLiteralExpr: {
            for (const auto& field : static_cast<const DataLiteralExpr&>(*node).fields) visit(field.second.get());
            return;
        }
        case NodeKind::DataUpdateExpr: {
            const auto& expression = static_cast<const DataUpdateExpr&>(*node);
            visit(expression.base.get());
            for (const auto& field : expression.fields) visit(field.second.get());
            return;
        }
        case NodeKind::MatchExpr: {
            const auto& expression = static_cast<const MatchExpr&>(*node);
            visit(expression.subject.get());
            for (const auto& arm : expression.arms) {
                visit(arm.guard.get());
                visit(arm.result.get());
            }
            return;
        }
        case NodeKind::LambdaExpr: {
            // A lambda's body is visited like any other child. Callers that
            // treat a lambda as a boundary simply do not recurse into it.
            const auto& lambda = static_cast<const LambdaExpr&>(*node);
            visit(lambda.hasExprBody ? lambda.exprBody.get() : lambda.blockBody.get());
            return;
        }

        // Leaves: nothing nested to report.
        case NodeKind::Literal:
        case NodeKind::Identifier:
        case NodeKind::ThisExpr:
        case NodeKind::MoveExpr:
        case NodeKind::BreakStmt:
        case NodeKind::ContinueStmt:
        default:
            return;
    }
}

} // namespace zl
