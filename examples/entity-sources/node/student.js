'use strict';

// fqdb:entity prefix=student/ key=id
class Student {
  id = 0;
  name = '';
  // fqdb:field type=sint32
  score = 0;
}
module.exports = {Student};
