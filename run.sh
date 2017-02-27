#make -j 8; make modules -j 8; make modules_install -j8; make install -j8
make -j8
if [ $? -eq 0 ]
then
  echo "Successfully made"
else
  echo "Could not do: make" >&2
  exit 1
fi
make modules_install 
if [ $? -eq 0 ]
then
  echo "Successfully made"
else
  echo "Could not do: make modules_install" >&2
  exit 1
fi
make install
