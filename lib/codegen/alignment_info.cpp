#include "triton/codegen/alignment_info.h"
#include "triton/ir/module.h"
#include "triton/ir/function.h"
#include "triton/ir/basic_block.h"
#include "triton/ir/instructions.h"
#include "triton/ir/type.h"

namespace triton {
namespace codegen{


template<class T>
inline T add_to_cache(ir::value *i, T value, std::map<ir::value*, T> &map) {
  return map[i] = value;
}


bool alignment_info::is_first_axis_unit(ir::value *x){
  if(x->get_type()->is_tile_ty())
    return x->get_type()->get_tile_shapes()[0]->get_value() == 1;
  else
    return true;
}

bool alignment_info::populate_is_constant(ir::value *v) {
  if(is_constant_.find(v) != is_constant_.end())
    return is_constant_.at(v);
  // helper for the cache
  auto cache = [this,v](bool value){ return add_to_cache(v, value, is_constant_); };
  // populate
  if(auto *x = dynamic_cast<ir::retile_inst*>(v)){
    ir::value *op = x->get_operand(0);
    populate_is_constant(op);
    if(is_first_axis_unit(op))
      return cache(true);
  }
  if(auto *x = dynamic_cast<ir::binary_operator*>(v)){
    bool lhs = populate_is_constant(x->get_operand(0));
    bool rhs = populate_is_constant(x->get_operand(1));
    return cache(lhs && rhs);
  }
  if(auto *x = dynamic_cast<ir::psi_inst*>(v)){
    bool value_true = populate_is_constant(x->get_value_true());
    bool value_false = populate_is_constant(x->get_value_false());
    return cache(value_true && value_false);
  }
  if(v->get_type()->is_tile_ty())
    return cache(false);
  if(auto *x = dynamic_cast<ir::phi_node*>(v)){
    // put a conservative initial value in phi node to avoid infinite recursion
    bool result = true;
    for(unsigned n = 0; n < x->get_num_incoming(); n++){
      ir::value* inc = x->get_incoming_value(n);
      if(is_constant_.find(inc) != is_constant_.end())
        result = is_constant_.at(inc);
    }
    cache(result);
    // recurse
    for(unsigned n = 0; n < x->get_num_incoming(); n++){
      ir::value* inc = x->get_incoming_value(n);
      result = result && populate_is_constant(inc);
    }
    return cache(result);
  }
  // scalars are always constant in the contiguous dimension
  return cache(true);
}

unsigned alignment_info::populate_max_contiguous(ir::value *v){
  if(max_contiguous_.find(v) != max_contiguous_.end())
    return max_contiguous_.at(v);
  // helper for the cache
  auto cache = [this,v](unsigned value){ return add_to_cache(v, value, max_contiguous_); };
  // populate
  if(!v->get_type()->is_tile_ty())
    return cache(1);
  auto shapes = v->get_type()->get_tile_shapes();
  if(dynamic_cast<ir::get_global_range_inst*>(v))
    return cache(shapes[0]->get_value());
  if(dynamic_cast<ir::constant_range*>(v))
    return cache(shapes[0]->get_value());
  if(auto *x = dynamic_cast<ir::retile_inst*>(v)){
    ir::value *op = x->get_operand(0);
    if(op->get_type()->is_tile_ty()){
      auto op_shapes = op->get_type()->get_tile_shapes();
      if(op_shapes[0] == shapes[0])
        return cache(populate_max_contiguous(op));
    }
    return cache(1);
  }
  if(auto *x = dynamic_cast<ir::binary_operator*>(v)){
    ir::value* lhs = x->get_operand(0);
    ir::value* rhs = x->get_operand(1);
    unsigned lhs_max_contiguous = populate_max_contiguous(lhs);
    unsigned rhs_max_contiguous = populate_max_contiguous(rhs);
    bool lhs_has_cst = populate_is_constant(lhs);
    bool rhs_has_cst = populate_is_constant(rhs);
    if(x->is_int_add_sub()){
      if(lhs_has_cst)
        return cache(rhs_max_contiguous);
      if(rhs_has_cst)
        return cache(lhs_max_contiguous);
    }
  }
  if(auto *x = dynamic_cast<ir::psi_inst*>(v)){
    int value_true = populate_max_contiguous(x->get_value_true());
    int value_false = populate_max_contiguous(x->get_value_false());
    return cache(std::min(value_true, value_false));
  }
  if(auto *x = dynamic_cast<ir::getelementptr_inst*>(v)){
    ir::value* lhs = x->get_operand(0);
    ir::value* rhs = x->get_operand(1);
    unsigned lhs_max_contiguous = populate_max_contiguous(lhs);
    unsigned rhs_max_contiguous = populate_max_contiguous(rhs);
    bool lhs_has_cst = populate_is_constant(lhs);
    bool rhs_has_cst = populate_is_constant(rhs);
    if(lhs_has_cst)
      return cache(rhs_max_contiguous);
    if(rhs_has_cst)
      return cache(lhs_max_contiguous);
  }
  if(auto *x = dynamic_cast<ir::phi_node*>(v)){
    // put a conservative initial value in phi node to avoid infinite recursion
    unsigned result = 1;
    for(unsigned n = 0; n < x->get_num_incoming(); n++){
      ir::value* inc = x->get_incoming_value(n);
      if(max_contiguous_.find(inc) != max_contiguous_.end())
        result = max_contiguous_.at(inc);
    }
    cache(result);
    // recurse
    for(unsigned n = 0; n < x->get_num_incoming(); n++){
      ir::value* inc = x->get_incoming_value(n);
      result = std::min(result, populate_max_contiguous(inc));
    }
    return cache(result);
  }
  return cache(1);
}

unsigned alignment_info::populate_starting_multiple(ir::value *v){
  if(starting_multiple_.find(v) != starting_multiple_.end())
    return starting_multiple_.at(v);
  auto cache = [this,v](unsigned value){ return add_to_cache(v, value, starting_multiple_); };
  // has metadata
  if(auto *x = dynamic_cast<ir::instruction*>(v)){
    unsigned multiple_of = x->get_metadata(ir::metadata::multiple_of);
    if(multiple_of > 0)
      return cache(multiple_of);
  }
  // arguments
  if(auto *x = dynamic_cast<ir::argument*>(v)){
    std::set<ir::attribute> attributes = x->get_parent()->get_attributes(x);
    for(auto attr: attributes){
      if(attr.get_kind() == ir::multiple_of)
        return cache(attr.get_value());
      if(attr.get_kind() == ir::aligned){
        ir::type* ty = x->get_type()->get_pointer_element_ty();
        int nbits  = ty->get_primitive_size_in_bits();
        int nbytes = nbits / 8;
        return cache(attr.get_value() / nbytes);
      }
    }
  }
  if(auto *x = dynamic_cast<ir::binary_operator*>(v)){
    int lhs = populate_starting_multiple(x->get_operand(0));
    int rhs = populate_starting_multiple(x->get_operand(1));
    if(x->is_int_mult())
      return cache(lhs * rhs);
    if(x->is_int_add_sub())
      return cache(std::min(lhs, rhs));
    if(x->is_int_div())
      return cache(std::max(lhs / rhs, 1));
    if(x->is_int_rem())
      return cache(std::max(lhs % rhs, 1));
    if(x->is_shl())
      return cache(lhs << rhs);
    if(x->is_shr())
      return cache(std::max(lhs >> rhs, 1));
  }
  if(auto *x = dynamic_cast<ir::getelementptr_inst*>(v)){
    int lhs = populate_starting_multiple(x->get_operand(0));
    int rhs = populate_starting_multiple(x->get_operand(1));
    return cache(std::min(lhs, rhs));
  }
  if(auto *x = dynamic_cast<ir::retile_inst*>(v)){
    int op = populate_starting_multiple(x->get_operand(0));
    return cache(op);
  }
  if(auto *x = dynamic_cast<ir::get_global_range_inst*>(v)){
    return cache(v->get_type()->get_tile_shapes()[0]->get_value());
  }
  if(auto *x = dynamic_cast<ir::psi_inst*>(v)){
    int value_true = populate_starting_multiple(x->get_value_true());
    int value_false = populate_starting_multiple(x->get_value_false());
    return cache(std::min(value_true, value_false));
  }
  if(auto *x = dynamic_cast<ir::phi_node*>(v)){
    // put a conservative initial value in phi node to avoid infinite recursion
    unsigned result = 1;
    for(unsigned n = 0; n < x->get_num_incoming(); n++){
      ir::value* inc = x->get_incoming_value(n);
      if(starting_multiple_.find(inc) != starting_multiple_.end())
        result = starting_multiple_.at(inc);
    }
    cache(result);
    // recurse
    for(unsigned n = 0; n < x->get_num_incoming(); n++){
      ir::value* inc = x->get_incoming_value(n);
      result = std::min(result, populate_starting_multiple(inc));
    }
    return cache(result);
  }
  // scalars
  if(!v->get_type()->is_tile_ty())
    return cache(1);
  // tiles
  auto shapes = v->get_type()->get_tile_shapes();
  unsigned result = 1;
  for(unsigned i = 0; i < shapes.size() - 1; i++)
    result *= shapes[i]->get_value();
  return cache(result);
}

unsigned alignment_info::get_starting_multiple(ir::value* v) const {
  return starting_multiple_.at(v);
}

unsigned alignment_info::get_max_contiguous(ir::value* v) const {
  return max_contiguous_.at(v);
}


void alignment_info::run(ir::module &mod) {
  // populate constant
  for(ir::function *fn: mod.get_function_list())
  for(ir::basic_block *block: fn->blocks())
  for(ir::instruction *i: block->get_inst_list()){
    populate_is_constant(i);
  }

  // populate starting multiple
  for(ir::function *fn: mod.get_function_list())
  for(ir::basic_block *block: fn->blocks())
  for(ir::instruction *i: block->get_inst_list()){
    populate_starting_multiple(i);
  }

  // populate maximum contiguous
  for(ir::function *fn: mod.get_function_list())
  for(ir::basic_block *block: fn->blocks())
  for(ir::instruction *i: block->get_inst_list()){
    populate_max_contiguous(i);
  }
}


}
}
