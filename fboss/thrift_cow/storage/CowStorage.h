// (c) Facebook, Inc. and its affiliates. Confidential and proprietary.

#pragma once

#include <fboss/thrift_cow/nodes/Serializer.h>
#include <fboss/thrift_cow/nodes/Types.h>
#include <fboss/thrift_cow/storage/Storage.h>
#include <fboss/thrift_cow/visitors/ExtendedPathVisitor.h>
#include <fboss/thrift_cow/visitors/PatchApplier.h>
#include <fboss/thrift_cow/visitors/PathVisitor.h>
#include <folly/logging/xlog.h>

namespace facebook::fboss::fsdb {

namespace detail {

std::optional<StorageError> parseTraverseResult(
    const thrift_cow::ThriftTraverseResult& traverseResult);

std::optional<StorageError> parsePatchResult(
    thrift_cow::PatchApplyResult patchResult,
    std::optional<std::pair<
        thrift_cow::pv_detail::PathIter,
        thrift_cow::pv_detail::PathIter>> errorPath);

} // namespace detail

template <typename Root, typename Node = thrift_cow::ThriftStructNode<Root>>
class CowStorage : public Storage<Root, CowStorage<Root, Node>> {
 public:
  using Base = Storage<Root, CowStorage<Root, Node>>;
  using RootT = Root;
  using StorageImpl = Node;
  using PathIter = typename Base::PathIter;
  using ExtPath = typename Base::ExtPath;
  using ExtPathIter = typename Base::ExtPathIter;

  template <typename T>
  explicit CowStorage(T&& root)
    requires(std::is_same_v<std::decay_t<T>, Root>)
      : root_(std::make_shared<StorageImpl>(std::forward<T>(root))) {}

  template <typename T>
  explicit CowStorage(std::shared_ptr<T> root)
    requires(std::is_same_v<std::decay_t<T>, Root>)
      : root_(root) {}

  template <typename T>
  explicit CowStorage(T&& storage)
    requires(std::is_same_v<std::decay_t<T>, std::shared_ptr<StorageImpl>>)
      : root_(std::forward<T>(storage)) {}

  bool isPublished_impl() {
    return root_->isPublished();
  }

  void publish_impl() {
    root_->publish();
  }

  using Base::get;
  using Base::get_encoded;
  using Base::get_encoded_extended;
  using Base::patch;
  using Base::remove;
  using Base::set;
  using Base::set_encoded;

  template <typename T>
  typename Base::template Result<T> get_impl(PathIter begin, PathIter end)
      const {
    T out;
    auto op =
        thrift_cow::pvlambda([&](auto& node, auto /* begin */, auto /* end */) {
          auto val = node.toThrift();
          if constexpr (std::is_assignable_v<decltype(out)&, decltype(val)>) {
            out = std::move(val);
          } else {
            throw std::runtime_error("Type mismatch");
          }
        });
    const auto& rootNode = *root_;
    auto traverseResult = thrift_cow::RootPathVisitor::visit(
        rootNode, begin, end, thrift_cow::PathVisitOptions::visitLeaf(), op);
    if (traverseResult) {
      return out;
    } else if (
        traverseResult.code() ==
        thrift_cow::ThriftTraverseResult::Code::VISITOR_EXCEPTION) {
      return folly::makeUnexpected(StorageError(
          StorageError::Code::TYPE_ERROR, traverseResult.toString()));
    } else {
      return folly::makeUnexpected(StorageError(
          StorageError::Code::INVALID_PATH, traverseResult.toString()));
    }
  }

  typename Base::template Result<OperState>
  get_encoded_impl(PathIter begin, PathIter end, OperProtocol protocol) const {
    OperState result;
    result.protocol() = protocol;
    result.metadata().emplace();
    const auto& rootNode = *root_;
    thrift_cow::GetEncodedPathVisitorOperator op(protocol);
    auto traverseResult = thrift_cow::RootPathVisitor::visit(
        rootNode, begin, end, thrift_cow::PathVisitOptions::visitLeaf(), op);
    if (traverseResult) {
      result.contents() = std::move(*op.val);
      result.protocol() = protocol;
      return result;
    } else if (
        traverseResult.code() ==
        thrift_cow::ThriftTraverseResult::Code::VISITOR_EXCEPTION) {
      return folly::makeUnexpected(StorageError(
          StorageError::Code::TYPE_ERROR, traverseResult.toString()));
    } else {
      return folly::makeUnexpected(StorageError(
          StorageError::Code::INVALID_PATH, traverseResult.toString()));
    }
  }

  std::vector<TaggedOperState> get_encoded_extended_impl(
      ExtPathIter begin,
      ExtPathIter end,
      OperProtocol protocol) const {
    std::vector<TaggedOperState> result;
    const auto& rootNode = *root_;
    thrift_cow::ExtPathVisitorOptions options;
    thrift_cow::RootExtendedPathVisitor::visit(
        rootNode, begin, end, options, [&](auto& path, auto& node) {
          using NodeT = typename folly::remove_cvref_t<decltype(node)>;
          if constexpr (!thrift_cow::is_cow_type_v<NodeT>) {
            throw std::runtime_error(fmt::format(
                "get_encoded_extended_impl: unexpected type with ExtendedPathVisitor::visit(path={})",
                folly::join("/", path)));
          }
          TaggedOperState state;
          state.path()->path() = path;
          state.state()->contents() = node.encode(protocol);
          state.state()->protocol() = protocol;
          result.emplace_back(std::move(state));
        });
    return result;
  }

  template <typename T>
  std::optional<StorageError>
  set_impl(PathIter begin, PathIter end, T&& value) {
    StorageImpl::modifyPath(&root_, begin, end);
    using ValueT = typename folly::remove_cvref_t<decltype(value)>;
    auto op =
        thrift_cow::pvlambda([&](auto& node, auto /*begin*/, auto /*end*/) {
          using NodeT = typename folly::remove_cvref_t<decltype(node)>;
          using TType = typename NodeT::ThriftType;
          if constexpr (std::is_same_v<ValueT, TType>) {
            node.fromThrift(std::forward<T>(value));
          } else {
            throw std::runtime_error("set: type mismatch for passed in path");
          }
        });
    auto traverseResult = thrift_cow::RootPathVisitor::visit(
        *root_, begin, end, thrift_cow::PathVisitOptions::visitLeaf(), op);
    if (!traverseResult) {
      XLOG(DBG1) << "Error:set: PathVisit(): " << traverseResult.toString();
    }
    return detail::parseTraverseResult(traverseResult);
  }

  std::optional<StorageError>
  set_encoded_impl(PathIter begin, PathIter end, const OperState& state) {
    auto modifyResult = StorageImpl::modifyPath(&root_, begin, end);
    if (!modifyResult) {
      XLOG(DBG1) << "Error:set_encoded: modifyPath("
                 << folly::join("/", begin, end)
                 << "): " << modifyResult.toString();
      return detail::parseTraverseResult(modifyResult);
    }
    thrift_cow::SetEncodedPathVisitorOperator op(
        *state.protocol(), *state.contents());
    thrift_cow::ThriftTraverseResult traverseResult =
        thrift_cow::RootPathVisitor::visit(
            *root_,
            begin,
            end,
            thrift_cow::PathVisitOptions::visitLeaf(true),
            op);
    if (!traverseResult) {
      XLOG(DBG1) << "Error:set_encoded: SetEncoded(path: "
                 << folly::join("/", begin, end)
                 << "): " << traverseResult.toString();
    }
    return detail::parseTraverseResult(traverseResult);
  }

  std::optional<StorageError> patch_impl(Patch&& patch) {
    auto begin = patch.basePath()->begin();
    auto end = patch.basePath()->end();
    auto modifyResult = StorageImpl::modifyPath(&root_, begin, end);
    if (!modifyResult) {
      XLOG(DBG1) << "Error:patch: modifyPath(" << folly::join("/", begin, end)
                 << "): " << modifyResult.toString();
      return detail::parseTraverseResult(modifyResult);
    }
    thrift_cow::PatchApplyResult patchResult;
    std::optional<std::pair<
        thrift_cow::pv_detail::PathIter,
        thrift_cow::pv_detail::PathIter>>
        errorPath;
    auto op = thrift_cow::pvlambda([&](auto& node,
                                       thrift_cow::pv_detail::PathIter begin,
                                       thrift_cow::pv_detail::PathIter end) {
      using NodeT = typename folly::remove_cvref_t<decltype(node)>;
      using TC = typename NodeT::TC;
      if constexpr (std::is_same_v<
                        typename NodeT::CowType,
                        thrift_cow::ThriftObject>) {
        patchResult = thrift_cow::PatchApplier<TC>::apply(
            node.toThrift(), std::move(*patch.patch()), *patch.protocol());
        XLOG(DBG5) << "Visited base path in Thrift obj. patch result "
                   << apache::thrift::util::enumNameSafe(patchResult);
      } else {
        patchResult = thrift_cow::PatchApplier<TC>::apply(
            node, std::move(*patch.patch()), *patch.protocol());
        XLOG(DBG5) << "Visited base path. patch result "
                   << apache::thrift::util::enumNameSafe(patchResult);
      }
      if (patchResult != thrift_cow::PatchApplyResult::OK) {
        *errorPath = std::make_pair(begin, end);
      }
    });
    auto visitResult = thrift_cow::RootPathVisitor::visit(
        *root_, begin, end, thrift_cow::PathVisitOptions::visitLeaf(), op);
    auto visitError = detail::parseTraverseResult(visitResult);
    if (visitError) {
      XLOG(DBG1) << "Error:patch: PathVisit(path: "
                 << folly::join("/", begin, end)
                 << "): " << visitResult.toString();
      return visitError;
    }
    return detail::parsePatchResult(patchResult, errorPath);
  }

  std::optional<StorageError> patch_impl(const fsdb::OperDelta& delta) {
    std::optional<StorageError> result;
    for (const auto& unit : *delta.changes()) {
      if (result) {
        break;
      }

      auto rawPath = *unit.path()->raw();
      // TODO: verify old state matches expected?

      if (unit.newState()) {
        OperState newState;
        newState.contents() = *unit.newState();
        newState.protocol() = *delta.protocol();
        result = this->set_encoded_impl(
            rawPath.begin(), rawPath.end(), std::move(newState));
      } else {
        result = this->remove_impl(rawPath.begin(), rawPath.end());
      }
    }
    return result;
  }

  std::optional<StorageError> patch_impl(
      const fsdb::TaggedOperState& taggedState) {
    std::optional<StorageError> result;
    auto rawPath = *taggedState.path()->path();
    OperState newState;
    newState.protocol() = *taggedState.state()->protocol();
    newState.contents() = *taggedState.state()->contents();

    result = this->set_encoded_impl(
        rawPath.begin(), rawPath.end(), std::move(newState));
    return result;
  }

  std::optional<StorageError> remove_impl(PathIter begin, PathIter end) {
    auto traverseResult = StorageImpl::removePath(&root_, begin, end);
    if (traverseResult) {
      return std::nullopt;
    } else {
      XLOG(DBG1) << "Error:removePath: (path: " << folly::join("/", begin, end)
                 << "): " << traverseResult.toString();
      if (traverseResult.code() ==
          thrift_cow::ThriftTraverseResult::Code::VISITOR_EXCEPTION) {
        return StorageError(
            StorageError::Code::TYPE_ERROR, traverseResult.toString());
      } else {
        return StorageError(
            StorageError::Code::INVALID_PATH, traverseResult.toString());
      }
    }
  }

  const std::shared_ptr<StorageImpl>& root() const {
    return root_;
  }
  std::shared_ptr<StorageImpl> root() {
    return root_;
  }

 private:
  std::shared_ptr<StorageImpl> root_;
};

template <typename Root, typename Node>
bool operator==(
    const CowStorage<Root, Node>& first,
    const CowStorage<Root, Node>& second) {
  return first.root() == second.root();
}

template <typename Root, typename Node>
bool operator!=(
    const CowStorage<Root, Node>& first,
    const CowStorage<Root, Node>& second) {
  return first.root() != second.root();
}

} // namespace facebook::fboss::fsdb
