#include "kv/engines.hpp"

#include <functional>
#include <utility>

namespace kv {

// ===========================================================================
// Sentinel-first red-black tree (CLRS, chapter 13).
//
// legacy/kvstore_rbtree.c allocated the sentinel with malloc() and initialised
// only `key` and `color`.  `left`, `right` and `parent` were therefore garbage,
// and the first time the delete fix-up looked at the sibling of a nil node it
// dereferenced that garbage.  Here the sentinel is a real member that points at
// itself, which is the same trick the Linux kernel rbtree uses.  That single
// change removes both the crash and the malloc/free pair.
// ===========================================================================

RBTreeEngine::RBTreeEngine() {
    nil_.left = &nil_;
    nil_.right = &nil_;
    nil_.parent = &nil_;
    nil_.color = Color::Black;
    root_ = &nil_;
}

RBTreeEngine::~RBTreeEngine() { clear(); }

RBTreeEngine::Node* RBTreeEngine::allocate_node() {
    void* memory = pool_.allocate(sizeof(Node), alignof(Node));
    return new (memory) Node();
}

void RBTreeEngine::release_node(Node* node) {
    node->~Node();
    pool_.deallocate(node, sizeof(Node), alignof(Node));
}

void RBTreeEngine::destroy_subtree(Node* node) {
    if (node == nil()) {
        return;
    }
    destroy_subtree(node->left);
    destroy_subtree(node->right);
    release_node(node);
}

void RBTreeEngine::clear() {
    destroy_subtree(root_);
    root_ = nil();
    size_ = 0;
}

void RBTreeEngine::left_rotate(Node* x) {
    Node* y = x->right;
    x->right = y->left;
    if (y->left != nil()) {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == nil()) {
        root_ = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

void RBTreeEngine::right_rotate(Node* x) {
    Node* y = x->left;
    x->left = y->right;
    if (y->right != nil()) {
        y->right->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == nil()) {
        root_ = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }
    y->right = x;
    x->parent = y;
}

void RBTreeEngine::insert_fixup(Node* z) {
    while (z->parent->color == Color::Red) {
        if (z->parent == z->parent->parent->left) {
            Node* uncle = z->parent->parent->right;
            if (uncle->color == Color::Red) {
                z->parent->parent->color = Color::Red;
                z->parent->color = Color::Black;
                uncle->color = Color::Black;
                z = z->parent->parent;
            } else {
                if (z == z->parent->right) {
                    z = z->parent;
                    left_rotate(z);
                }
                z->parent->color = Color::Black;
                z->parent->parent->color = Color::Red;
                right_rotate(z->parent->parent);
            }
        } else {
            Node* uncle = z->parent->parent->left;
            if (uncle->color == Color::Red) {
                z->parent->parent->color = Color::Red;
                z->parent->color = Color::Black;
                uncle->color = Color::Black;
                z = z->parent->parent;
            } else {
                if (z == z->parent->left) {
                    z = z->parent;
                    right_rotate(z);
                }
                z->parent->color = Color::Black;
                z->parent->parent->color = Color::Red;
                left_rotate(z->parent->parent);
            }
        }
    }
    root_->color = Color::Black;
}

void RBTreeEngine::insert_node(Node* z) {
    Node* parent = nil();
    Node* cursor = root_;
    while (cursor != nil()) {
        parent = cursor;
        if (z->key < cursor->key) {
            cursor = cursor->left;
        } else {
            cursor = cursor->right;
        }
    }
    z->parent = parent;
    if (parent == nil()) {
        root_ = z;
    } else if (z->key < parent->key) {
        parent->left = z;
    } else {
        parent->right = z;
    }
    z->left = nil();
    z->right = nil();
    z->color = Color::Red;
    // size_ is bumped by the caller only after insert_fixup, so the invariant
    // checker never observes a half-built tree.
    insert_fixup(z);
}

RBTreeEngine::Node* RBTreeEngine::minimum(Node* node) const {
    while (node->left != nil()) {
        node = node->left;
    }
    return node;
}

void RBTreeEngine::transplant(Node* u, Node* v) {
    if (u->parent == nil()) {
        root_ = v;
    } else if (u == u->parent->left) {
        u->parent->left = v;
    } else {
        u->parent->right = v;
    }
    v->parent = u->parent;
}

void RBTreeEngine::erase_fixup(Node* x) {
    while (x != root_ && x->color == Color::Black) {
        if (x == x->parent->left) {
            Node* sibling = x->parent->right;
            if (sibling->color == Color::Red) {
                sibling->color = Color::Black;
                x->parent->color = Color::Red;
                left_rotate(x->parent);
                sibling = x->parent->right;
            }
            if (sibling->left->color == Color::Black && sibling->right->color == Color::Black) {
                sibling->color = Color::Red;
                x = x->parent;
            } else {
                if (sibling->right->color == Color::Black) {
                    sibling->left->color = Color::Black;
                    sibling->color = Color::Red;
                    right_rotate(sibling);
                    sibling = x->parent->right;
                }
                sibling->color = x->parent->color;
                x->parent->color = Color::Black;
                sibling->right->color = Color::Black;
                left_rotate(x->parent);
                x = root_;
            }
        } else {
            Node* sibling = x->parent->left;
            if (sibling->color == Color::Red) {
                sibling->color = Color::Black;
                x->parent->color = Color::Red;
                right_rotate(x->parent);
                sibling = x->parent->left;
            }
            if (sibling->right->color == Color::Black && sibling->left->color == Color::Black) {
                sibling->color = Color::Red;
                x = x->parent;
            } else {
                if (sibling->left->color == Color::Black) {
                    sibling->right->color = Color::Black;
                    sibling->color = Color::Red;
                    left_rotate(sibling);
                    sibling = x->parent->left;
                }
                sibling->color = x->parent->color;
                x->parent->color = Color::Black;
                sibling->left->color = Color::Black;
                right_rotate(x->parent);
                x = root_;
            }
        }
    }
    x->color = Color::Black;
}

void RBTreeEngine::erase_node(Node* z) {
    Node* removed = z;
    Color removed_color = removed->color;
    Node* child = nil();

    if (z->left == nil()) {
        child = z->right;
        transplant(z, z->right);
    } else if (z->right == nil()) {
        child = z->left;
        transplant(z, z->left);
    } else {
        removed = minimum(z->right);
        removed_color = removed->color;
        child = removed->right;
        if (removed->parent == z) {
            child->parent = removed;
        } else {
            transplant(removed, removed->right);
            removed->right = z->right;
            removed->right->parent = removed;
        }
        transplant(z, removed);
        removed->left = z->left;
        removed->left->parent = removed;
        removed->color = z->color;
    }

    if (removed_color == Color::Black) {
        erase_fixup(child);
    }
    release_node(z);
}

RBTreeEngine::Node* RBTreeEngine::find_node(std::string_view key) const {
    Node* cursor = root_;
    while (cursor != nil()) {
        if (equals(cursor->key, key)) {
            return cursor;
        }
        if (key < cursor->key) {
            cursor = cursor->left;
        } else {
            cursor = cursor->right;
        }
    }
    return nullptr;
}

Status RBTreeEngine::put(std::string_view key, std::string_view value) {
    if (key.empty()) {
        return Status::InvalidArgument;
    }
    if (Node* existing = find_node(key)) {
        // The C version allocated a new node, called rbtree_insert(), which
        // returned early on a duplicate key, leaked the node and still
        // incremented the counter.
        existing->value.assign(value);
        return Status::Ok;
    }
    Node* node = allocate_node();
    node->key.assign(key);
    node->value.assign(value);
    insert_node(node);
    ++size_;
    return Status::Ok;
}

std::optional<std::string> RBTreeEngine::get(std::string_view key) const {
    const Node* node = find_node(key);
    if (node == nullptr) {
        return std::nullopt;
    }
    return node->value;
}

Status RBTreeEngine::erase(std::string_view key) {
    Node* node = find_node(key);
    if (node == nullptr) {
        return Status::NotFound;
    }
    erase_node(node);
    --size_;
    return Status::Ok;
}

Status RBTreeEngine::update(std::string_view key, std::string_view value) {
    Node* node = find_node(key);
    if (node == nullptr) {
        return Status::NotFound;
    }
    node->value.assign(value);
    return Status::Ok;
}

// In-order traversal, so for_each() yields keys in sorted order.  Iterative
// code would avoid the recursion, but the depth is O(log n) by construction.
void RBTreeEngine::walk(const Node* node, const VisitFn& visit) const {
    if (node == nil()) {
        return;
    }
    walk(node->left, visit);
    visit(node->key, node->value);
    walk(node->right, visit);
}

void RBTreeEngine::for_each(const VisitFn& visit) const { walk(root_, visit); }

bool RBTreeEngine::check_invariants() const {
    bool ok = true;
    std::size_t counted = 0;

    // Returns the black height of the subtree (counting nil as one black
    // level), or 0 when a violation was found.
    std::function<int(const Node*, const Node*, const Node*, const Node*)> walk =
        [&](const Node* node, const Node* parent, const Node* lower, const Node* upper) -> int {
        if (node == nil()) {
            return 1;
        }
        if (node->parent != parent) {
            ok = false;
            return 0;
        }
        const std::string_view key = node->key;
        if (lower != nullptr && !(std::string_view(lower->key) < key)) {
            ok = false;  // violates BST ordering
            return 0;
        }
        if (upper != nullptr && !(key < std::string_view(upper->key))) {
            ok = false;
            return 0;
        }
        ++counted;
        if (node->color == Color::Red && (is_red(node->left) || is_red(node->right))) {
            ok = false;  // red node with a red child
            return 0;
        }
        const int left_height = walk(node->left, node, lower, node);
        const int right_height = walk(node->right, node, node, upper);
        if (!ok || left_height != right_height) {
            ok = false;
            return 0;
        }
        return left_height + (node->color == Color::Black ? 1 : 0);
    };

    const int root_height = walk(root_, nil(), nullptr, nullptr);
    if (is_red(root_)) {
        ok = false;  // the root must be black
    }
    if (counted != size_) {
        ok = false;  // counter drifted from the real node count
    }
    return ok && root_height > 0;
}

int RBTreeEngine::height() const {
    std::function<int(const Node*)> depth = [&](const Node* node) -> int {
        if (node == nil()) {
            return 0;
        }
        const int left = depth(node->left);
        const int right = depth(node->right);
        return 1 + (left > right ? left : right);
    };
    return depth(root_);
}

}  // namespace kv
