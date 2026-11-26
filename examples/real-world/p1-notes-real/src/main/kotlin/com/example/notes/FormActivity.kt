package com.example.notes

import android.app.Activity
import android.os.Bundle
import android.widget.Button
import android.widget.EditText

class FormActivity : Activity() {
  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.activity_form)
    val id = intent.getStringExtra("note_id")
    if (id != null) {
      findViewById<EditText>(R.id.title_field).setText(id)
    }
    findViewById<Button>(R.id.save_button).setOnClickListener { finish() }
    findViewById<Button>(R.id.cancel_button).setOnClickListener { finish() }
  }
}
