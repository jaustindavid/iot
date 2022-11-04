#ifndef TinyQueue_h
#define TinyQueue_h


#ifndef FTINYQUEUE_SIZE
// 50 ints
#define FTINYQUEUE_SIZE 50
#endif

template <class T> 
class TinyQueue {
  public:
    TinyQueue(byte);
    void reset();
    void enqueue(const T value);
    T dequeue();
    // void dump(Print & printer);
    bool isEmpty();
    int count();

	
  private:
    byte TINYQUEUE_SIZE;
    int _head, _tail, _count;
    // T _queue[TINYQUEUE_SIZE + 1];
    T* _queue;
}; // class TinyQueue


#define incrment(M) ((M + 1) % TINYQUEUE_SIZE)

/*

// helper function --- increment an index, wrap if needed
int incrment(const int index) {
    int ret = index + 1;
    if (ret >= TINYQUEUE_SIZE) {
        return 0;
    } else {
        return ret;
    }
} // increment
*/

// constructor
template <class T>
TinyQueue<T>::TinyQueue(byte qsize) {
    TINYQUEUE_SIZE = qsize;
    _queue = (T*)malloc(sizeof(T) * (TINYQUEUE_SIZE + 1));
    reset();
} // TinyQueue<T>()


// reset the Queue to "empty"
template <typename T>
void TinyQueue<T>::reset() {
  _head = _tail = _count = 0;
} // TinyQueue::reset()


/*
 * enqueue(value) to the front of the queue
 *
 * silently ejects the last element if the queue is full
 */
template <class T>
void TinyQueue<T>::enqueue(const T value) {
  _queue[_tail] = value;
  _tail = incrment(_tail);
  _count += 1;
  if (_tail == _head) {
        // silent discard, but track the sum
        _head = incrment(_head);
        _count -= 1;
  }
} // TinyQueue::enqueue(value)


/*
 * take a value from the end of the queue
 *
 * returns -1 if the queue is empty.
 */
template <class T> 
T TinyQueue<T>::dequeue() {
  T value;
  if (! isEmpty()) {
    value = _queue[_head];
    _head = incrment(_head);
    _count -= 1;
    return value;
  } else {
    return (T)-1;
  }
} // int TinyQueue::dequeue()


// returns true if the queue isEmpty
template <typename T>
bool TinyQueue<T>::isEmpty() {
  return _head == _tail;
} // boolean TinyQueue::isEmpty()


template <class T> 
int TinyQueue<T>::count() {
    return _count;
}

#endif
