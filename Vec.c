/* CIS 5480 Homework 0
Author: Shaho Solaman
Purpose: Implementation of a general purpose vector of type vec
*/

#include "./Vec.h"
#include <stdlib.h>
#include "./panic.h"

Vec vec_new(size_t initial_capacity, ptr_dtor_fn ele_dtor_fn) {
  ptr_t* data;
  // empty vector initialized if capacity is 0
  if (initial_capacity == 0) {
    data = NULL;
  } else {
    // allocating memory for vector's void* pointers
    data = (ptr_t*)malloc(sizeof(ptr_t) * initial_capacity);
    if (data == NULL) {
      panic("vec_new error: malloc failed\n");
    }
  }
  // initialize vector strut Vec and all fields
  Vec new_vec;
  new_vec.data = data;
  new_vec.length = 0;
  new_vec.capacity = initial_capacity;
  new_vec.ele_dtor_fn = ele_dtor_fn;
  return new_vec;
}

ptr_t vec_get(Vec* self, size_t index) {
  if (index >= self->length) {
    panic("vec_get error: index out of bounds\n");
  }
  return self->data[index];
}

void vec_set(Vec* self, size_t index, ptr_t new_ele) {
  if (index >= self->length) {
    panic("vec_set error: index out of bounds\n");
  }
  if (self->ele_dtor_fn != NULL) {
    self->ele_dtor_fn(
        self->data[index]);  // destroy the old element before overwriting
  }
  self->data[index] = new_ele;
}

void vec_push_back(Vec* self, ptr_t new_ele) {
  if (self->length >= self->capacity) {
    // vector resize if capacity is > 1, becomes 1 if capacity is 0
    size_t new_capacity = (self->capacity == 0) ? 1 : self->capacity * 2;
    vec_resize(self, new_capacity);
  }
  self->data[self->length] = new_ele;
  self->length++;
}

bool vec_pop_back(Vec* self) {
  if (self->length == 0) {
    return false;  // nothing to remove; return false
  }
  // destruct last element and decrement length
  if (self->ele_dtor_fn != NULL) {
    self->ele_dtor_fn(self->data[self->length - 1]);
  }
  self->length--;
  return true;
}

void vec_insert(Vec* self, size_t index, ptr_t new_ele) {
  if (index > self->length) {
    panic("vec_insert error: index out of bounds\n");
  }
  // resize if no more capacity left
  if (self->length >= self->capacity) {
    size_t new_capacity = (self->capacity == 0) ? 1 : self->capacity * 2;
    vec_resize(self, new_capacity);
  }
  // shift all elements to the right 1 spot except those before index
  for (size_t i = self->length; i > index; i--) {
    self->data[i] = self->data[i - 1];
  }
  self->data[index] = new_ele;
  self->length++;
}

void vec_erase(Vec* self, size_t index) {
  if (index >= self->length) {
    panic("vec_erase error: index out of bounds\n");
  }
  if (self->ele_dtor_fn != NULL) {
    self->ele_dtor_fn(self->data[index]);  // destruct element at index
  }
  for (size_t i = index; i < self->length - 1; i++) {  // shift down by one
    self->data[i] = self->data[i + 1];
  }
  self->length--;
}

void vec_clear(Vec* self) {
  if (self->ele_dtor_fn !=
      NULL) {  // call destructor on all elements if it exists
    for (size_t i = 0; i < self->length; i++) {
      self->ele_dtor_fn(self->data[i]);
    }
  }
  self->length = 0;
}

void vec_destroy(Vec* self) {
  vec_clear(self);
  self->capacity = 0;
  free((void*)self
           ->data);  // deallocate memory for vec data before setting it to NULL
  self->data = NULL;
}

void vec_resize(Vec* self, size_t new_capacity) {
  if (new_capacity >
      self->length) {  // resize only occurs if new_capacity is more than length
    ptr_t* new_data = (ptr_t*)malloc(sizeof(ptr_t) * new_capacity);
    if (new_data == NULL) {
      panic("vec_resize error: malloc failed\n");
    }
    for (size_t i = 0; i < self->length; i++) {
      new_data[i] = self->data[i];
    }
    free((void*)self->data);  // after copying every element to new malloc
                              // block, free old one
    self->data = new_data;    // update fields
    self->capacity = new_capacity;
  }
}
