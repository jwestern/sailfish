use std::os::raw::{c_void, c_ulong};
use std::mem;

extern "C" {
    pub fn gpu_malloc(size: c_ulong) -> *mut c_void;
    pub fn gpu_free(ptr: *mut c_void);
    pub fn gpu_memcpy_htod(dst: *mut c_void, src: *const c_void, size: c_ulong);
    pub fn gpu_memcpy_dtoh(dst: *mut c_void, src: *const c_void, size: c_ulong);
    pub fn gpu_memcpy_dtod(dst: *mut c_void, src: *const c_void, size: c_ulong);
}

pub struct DeviceVec<T: Copy> {
    ptr: *mut T,
    len: usize,
}

impl<T: Copy> DeviceVec<T> {
    pub fn len(&self) -> usize {
        self.len
    }
    pub fn as_device_ptr(&self) -> *const T {
        self.ptr
    }
    pub fn as_mut_device_ptr(&mut self) -> *mut T {
        self.ptr
    }
}

impl<T: Copy> From<&[T]> for DeviceVec<T> {
    fn from(slice: &[T]) -> Self {
        let bytes = (slice.len() * mem::size_of::<T>()) as c_ulong;
        unsafe {
            let ptr = gpu_malloc(bytes);
            gpu_memcpy_htod(ptr, slice.as_ptr() as *const c_void, bytes);
            Self {
                ptr: ptr as *mut T,
                len: slice.len(),
            }
        }
    }
}

impl<T: Copy> From<&Vec<T>> for DeviceVec<T> {
    fn from(vec: &Vec<T>) -> Self {
        vec.as_slice().into()
    }
}

impl<T: Copy> From<&DeviceVec<T>> for Vec<T> where T: Default {
    fn from(dvec: &DeviceVec<T>) -> Self {
        let mut hvec = vec![T::default(); dvec.len()];
        let bytes = (dvec.len() * mem::size_of::<T>()) as c_ulong;
        unsafe {
            gpu_memcpy_dtoh(hvec.as_mut_ptr() as *mut c_void, dvec.ptr as *const c_void, bytes)
        };
        hvec
    }
}

impl<T: Copy> Drop for DeviceVec<T> {
    fn drop(&mut self) {
        unsafe {
            gpu_free(self.ptr as *mut c_void)
        }
    }
}

impl<T: Copy> Clone for DeviceVec<T> {
    fn clone(&self) -> Self {
        let bytes = (self.len * mem::size_of::<T>()) as c_ulong;
        unsafe {
            let ptr = gpu_malloc(bytes);
            gpu_memcpy_dtod(ptr, self.ptr as *const c_void, bytes);
            Self {
                ptr: ptr as *mut T,
                len: self.len,
            }            
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip() {
        let hvec: Vec<_> = (0..100).collect();
        let dvec = DeviceVec::from(&hvec);
        assert_eq!(hvec, Vec::from(&dvec));
    }
}